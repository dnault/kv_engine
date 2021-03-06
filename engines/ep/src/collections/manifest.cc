/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2016 Couchbase, Inc
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

#include "collections/manifest.h"
#include "bucket_logger.h"
#include "collections/collections_constants.h"
#include "collections/collections_types.h"
#include "ep_engine.h"
#include "kv_bucket.h"
#include "statistics/collector.h"
#include "utility.h"

#include <json_utilities.h>

#include <memcached/engine_error.h>
#include <nlohmann/json.hpp>
#include <platform/checked_snprintf.h>
#include <gsl/gsl>

#include <cctype>
#include <cstring>
#include <iostream>
#include <sstream>

namespace Collections {

// strings used in JSON parsing
static constexpr char const* ScopesKey = "scopes";
static constexpr nlohmann::json::value_t ScopesType =
        nlohmann::json::value_t::array;
static constexpr char const* CollectionsKey = "collections";
static constexpr char const* NameKey = "name";
static constexpr nlohmann::json::value_t NameType =
        nlohmann::json::value_t::string;
static constexpr char const* UidKey = "uid";
static constexpr char const* MaxTtlKey = "maxTTL";
static constexpr nlohmann::json::value_t MaxTtlType =
        nlohmann::json::value_t::number_unsigned;

/**
 * Get json sub-object from the json object for key and check the type.
 * @param json The parent object in which to find key.
 * @param key The key to look for.
 * @param expectedType The type the found object must be.
 * @return A json object for key.
 * @throws std::invalid_argument if key is not found or the wrong type.
 */
nlohmann::json getJsonObject(const nlohmann::json& object,
                             const std::string& key,
                             nlohmann::json::value_t expectedType);

/**
 * Constructor helper function, throws invalid_argument with a string
 * indicating if the expectedType.
 *
 * @param errorKey the JSON key being looked up
 * @param object object to check
 * @param expectedType the type we expect object to be
 * @throws std::invalid_argument if !expectedType
 */
static void throwIfWrongType(const std::string& errorKey,
                             const nlohmann::json& object,
                             nlohmann::json::value_t expectedType);

Manifest::Manifest(std::string_view json)
    : defaultCollectionExists(false), scopes(), collections(), uid(0) {
    auto throwInvalid = [](const std::string& detail) {
        throw std::invalid_argument("Manifest::Manifest: " + detail);
    };

    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(json);
    } catch (const nlohmann::json::exception& e) {
        throwInvalid("nlohmann cannot parse json:" + std::string(json) +
                     ", e:" + e.what());
    }

    // Read the Manifest UID e.g. "uid" : "5fa1"
    auto jsonUid = getJsonObject(parsed, UidKey, UidType);
    uid = makeUid(jsonUid.get<std::string>());

    // Read the scopes within the Manifest
    auto scopes = getJsonObject(parsed, ScopesKey, ScopesType);

    for (const auto& scope : scopes) {
        throwIfWrongType(
                std::string(ScopesKey), scope, nlohmann::json::value_t::object);

        auto name = getJsonObject(scope, NameKey, NameType);
        auto uid = getJsonObject(scope, UidKey, UidType);

        auto nameValue = name.get<std::string>();
        if (!validName(nameValue)) {
            throwInvalid("scope name: " + nameValue + " is not valid.");
        }

        // Construction of ScopeID checks for invalid values
        ScopeID uidValue = makeScopeID(uid.get<std::string>());

        // 1) Default scope has an expected name.
        // 2) Scope identifiers must be unique.
        // 3) Scope names must be unique.
        if (uidValue.isDefaultScope() && nameValue != DefaultScopeIdentifier) {
            throwInvalid("default scope with wrong name:" + nameValue);
        } else if (this->scopes.count(uidValue) > 0) {
            // Scope uids must be unique
            throwInvalid("duplicate scope uid:" + uidValue.to_string() +
                         ", name:" + nameValue);
        }

        // iterate scopes and compare names, fail for a match.
        for (const auto& itr : this->scopes) {
            if (itr.second.name == nameValue) {
                throwInvalid("duplicate scope name:" + uidValue.to_string() +
                             ", name:" + nameValue);
            }
        }

        std::vector<CollectionEntry> scopeCollections = {};

        // Read the collections within this scope
        auto collections =
                getJsonObject(scope, CollectionsKey, CollectionsType);

        for (const auto& collection : collections) {
            throwIfWrongType(std::string(CollectionsKey),
                             collection,
                             nlohmann::json::value_t::object);

            auto cname = getJsonObject(collection, NameKey, NameType);
            auto cuid = getJsonObject(collection, UidKey, UidType);
            auto cmaxttl = cb::getOptionalJsonObject(
                    collection, MaxTtlKey, MaxTtlType);

            auto cnameValue = cname.get<std::string>();
            if (!validName(cnameValue)) {
                throwInvalid("collection name:" + cnameValue + " is not valid");
            }

            CollectionID cuidValue = makeCollectionID(cuid.get<std::string>());

            // 1) The default collection must be within the default scope and
            // have the expected name.
            // 2) The constructor of CollectionID checked for invalid values,
            // but we need to check to ensure System (1) wasn't present in the
            // Manifest.
            // 3) Collection identifiers must be unique.
            // 4) Collection names must be unique within the scope.
            if (cuidValue.isDefaultCollection()) {
                if (cnameValue != DefaultCollectionIdentifier) {
                    throwInvalid(
                            "the default collection name is unexpected name:" +
                            cnameValue);
                } else if (!uidValue.isDefaultScope()) {
                    throwInvalid(
                            "the default collection is not in the default "
                            "scope");
                }
            } else if (invalidCollectionID(cuidValue)) {
                throwInvalid("collection uid: " + cuidValue.to_string() +
                             " is not valid.");
            } else if (this->collections.count(cuidValue) > 0) {
                throwInvalid("duplicate collection uid:" +
                             cuidValue.to_string() + ", name: " + cnameValue);
            }

            // Collection names must be unique within the scope
            for (const auto& itr : scopeCollections) {
                auto existingCollection = this->collections.find(itr.id);
                if (existingCollection != this->collections.end()) {
                    if (existingCollection->second.name == cnameValue) {
                        throwInvalid("duplicate collection name:" +
                                     cuidValue.to_string() +
                                     ", name: " + cnameValue);
                    }
                }
            }

            cb::ExpiryLimit maxTtl;
            if (cmaxttl) {
                // Don't exceed 32-bit max
                auto value = cmaxttl.value().get<uint64_t>();
                if (value > std::numeric_limits<uint32_t>::max()) {
                    throwInvalid("maxTTL:" + std::to_string(value));
                }
                maxTtl = std::chrono::seconds(value);
            }

            enableDefaultCollection(cuidValue);
            this->collections.emplace(std::make_pair(
                    cuidValue, Collection{uidValue, cnameValue}));
            scopeCollections.push_back({cuidValue, maxTtl});
        }

        this->scopes.emplace(uidValue,
                             Scope{nameValue, std::move(scopeCollections)});
    }

    // Final checks...
    // uid of 0 -> this must be the 'epoch' state
    // else no scopes is invalid and we must always have default scope
    if (uid == 0 && !isEpoch()) {
        throwInvalid("uid of 0 but not the expected 'epoch' manifest");
    } else if (this->scopes.empty()) {
        throwInvalid("no scopes were defined in the manifest");
    } else if (findScope(ScopeID::Default) == this->scopes.end()) {
        throwInvalid("the default scope was not defined");
    }
}

nlohmann::json getJsonObject(const nlohmann::json& object,
                             const std::string& key,
                             nlohmann::json::value_t expectedType) {
    return cb::getJsonObject(object, key, expectedType, "Manifest");
}

void throwIfWrongType(const std::string& errorKey,
                      const nlohmann::json& object,
                      nlohmann::json::value_t expectedType) {
    cb::throwIfWrongType(errorKey, object, expectedType, "Manifest");
}

void Manifest::enableDefaultCollection(CollectionID identifier) {
    if (identifier == CollectionID::Default) {
        defaultCollectionExists = true;
    }
}

bool Manifest::validName(std::string_view name) {
    // $ prefix is currently reserved for future use
    // Name cannot be empty
    if (name.empty() || name.size() > MaxCollectionNameSize || name[0] == '$') {
        return false;
    }
    // Check rest of the characters for validity
    for (const auto& c : name) {
        // Collection names are allowed to contain
        // A-Z, a-z, 0-9 and _ - % $
        // system collections are _ prefixed, but not enforced here
        if (!(std::isdigit(c) || std::isalpha(c) || c == '_' || c == '-' ||
              c == '%' || c == '$')) {
            return false;
        }
    }
    return true;
}

bool Manifest::invalidCollectionID(CollectionID identifier) {
    // System cannot appear in a manifest
    return identifier == CollectionID::System;
}

nlohmann::json Manifest::toJson(
        const Collections::IsVisibleFunction& isVisible) const {
    nlohmann::json manifest;
    manifest["uid"] = fmt::format("{0:x}", uid);
    manifest["scopes"] = nlohmann::json::array();

    // scope check is correct to see an empty scope
    // collection check is correct as well, if you have no visible collections
    // and no access to the scope - no scope

    for (const auto& s : scopes) {
        nlohmann::json scope;
        scope["collections"] = nlohmann::json::array();
        bool visible = isVisible(s.first, {});
        for (const auto& c : s.second.collections) {
            // Include if the collection is visible
            if (isVisible(s.first, c.id)) {
                nlohmann::json collection;
                collection["name"] = collections.at(c.id).name;
                collection["uid"] = fmt::format("{0:x}", uint32_t{c.id});
                if (c.maxTtl) {
                    collection["maxTTL"] = c.maxTtl.value().count();
                }
                scope["collections"].push_back(collection);
            }
        }
        if (!scope["collections"].empty() || visible) {
            scope["name"] = s.second.name;
            scope["uid"] = fmt::format("{0:x}", s.first);
            manifest["scopes"].push_back(scope);
        }
    }
    return manifest;
}

void Manifest::addCollectionStats(KVBucket& bucket,
                                  const void* cookie,
                                  const AddStatFn& add_stat) const {
    const auto addStat = [&cookie, &add_stat](std::string_view key,
                                              const auto& value) {
        fmt::memory_buffer valueBuf;
        format_to(valueBuf, "{}", value);
        add_stat({key.data(), key.size()}, {valueBuf.data(), valueBuf.size()}, cookie);
    };

    try {
        // manifest_uid is always permitted (e.g. get_collections_manifest
        // exposes this too). It reveals nothing about scopes or collections but
        // is useful for assisting in access failures
        addStat("manifest_uid", uid);
        for (const auto& scope : scopes) {
            for (const auto& entry : scope.second.collections) {
                // The inclusion of each collection requires an appropriate
                // privilege
                if (bucket.getEPEngine().testPrivilege(
                            cookie,
                            cb::rbac::Privilege::SimpleStats,
                            scope.first,
                            entry.id) != cb::engine_errc::success) {
                    continue; // skip this collection
                }
                const auto& name = collections.at(entry.id).name;
                const auto cid = entry.id.to_string();

                fmt::memory_buffer key;
                format_to(key, "{}:{}:name", scope.first.to_string(), cid);
                addStat({key.data(), key.size()}, name);

                if (entry.maxTtl) {
                    key.resize(0);
                    format_to(
                            key, "{}:{}:maxTTL", scope.first.to_string(), cid);
                    addStat({key.data(), key.size()}, entry.maxTtl->count());
                }
            }
        }
    } catch (const std::exception& e) {
        EP_LOG_WARN(
                "Manifest::addCollectionStats failed to build stats "
                "exception:{}",
                e.what());
    }
}

void Manifest::addScopeStats(KVBucket& bucket,
                             const void* cookie,
                             const AddStatFn& add_stat) const {
    try {
        fmt::memory_buffer buf;
        // manifest_uid is always permitted (e.g. get_collections_manifest
        // exposes this too). It reveals nothing about scopes or collections but
        // is useful for assisting in access failures
        add_casted_stat("manifest_uid", uid, add_stat, cookie);

        for (const auto& entry : scopes) {
            // The inclusion of each scope requires an appropriate
            // privilege
            if (bucket.getEPEngine().testPrivilege(
                        cookie,
                        cb::rbac::Privilege::SimpleStats,
                        entry.first,
                        {}) != cb::engine_errc::success) {
                continue; // skip this scope
            }
            const auto sid = entry.first.to_string();
            const auto name = entry.second.name;

            buf.resize(0);
            fmt::format_to(buf, "{}:name", sid);
            add_casted_stat({buf.data(), buf.size()}, name, add_stat, cookie);

            buf.resize(0);
            fmt::format_to(buf, "{}:collections", sid);
            add_casted_stat<unsigned long>({buf.data(), buf.size()},
                                           entry.second.collections.size(),
                                           add_stat,
                                           cookie);
            // add each collection name and id
            for (const auto& colEntry : entry.second.collections) {
                auto colName = findCollection(colEntry.id)->second.name;

                buf.resize(0);
                fmt::format_to(buf, "{}:{}:name", sid, colEntry.id.to_string());
                add_casted_stat(
                        {buf.data(), buf.size()}, colName, add_stat, cookie);
            }
        }
    } catch (const std::exception& e) {
        EP_LOG_WARN(
                "Manifest::addScopeStats failed to build stats "
                "exception:{}",
                e.what());
    }
}

std::optional<CollectionID> Manifest::getCollectionID(
        ScopeID scope, std::string_view path) const {
    int pos = path.find_first_of('.');
    auto collection = path.substr(pos + 1);

    // Empty collection part of the path means default collection.
    if (collection.empty()) {
        collection = DefaultCollectionIdentifier;
    }

    if (!validName(collection)) {
        throw cb::engine_error(cb::engine_errc::invalid_arguments,
                               "Manifest::getCollectionID invalid collection:" +
                                       std::string(collection));
    }

    auto scopeItr = scopes.find(scope);
    if (scopeItr == scopes.end()) {
        // Assumption is that a valid scope will be given because it was looked
        // up first via getScopeId(path) - so it is invalid to give a bad scope.
        throw std::invalid_argument(
                "Manifest::getCollectionID given unknown scope:" +
                scope.to_string());
    }
    for (const auto& c : scopeItr->second.collections) {
        auto cItr = collections.find(c.id);
        if (cItr != collections.end()) {
            if (cItr->second.name == collection) {
                return c.id;
            }
        }
    }

    return {};
}

std::optional<ScopeID> Manifest::getScopeID(std::string_view path) const {
    int pos = path.find_first_of('.');
    auto scope = path.substr(0, pos);

    // Empty scope part of the path means default scope.
    if (scope.empty()) {
        scope = DefaultScopeIdentifier;
    }

    if (!(validName(scope))) {
        throw cb::engine_error(
                cb::engine_errc::invalid_arguments,
                "Manifest::getScopeID invalid scope:" + std::string(scope));
    }

    for (const auto& s : scopes) {
        if (s.second.name == scope) {
            return s.first;
        }
    }

    return {};
}

std::optional<ScopeID> Manifest::getScopeID(const DocKey& key) const {
    if (key.isInDefaultCollection() && defaultCollectionExists) {
        return ScopeID{ScopeID::Default};
    } else {
        auto itr = collections.find(key.getCollectionID());
        if (itr != collections.end()) {
            return itr->second.sid;
        }
    }
    return {};
}

std::optional<ScopeID> Manifest::getScopeID(CollectionID cid) const {
    if (cid.isDefaultCollection() && defaultCollectionExists) {
        return ScopeID{ScopeID::Default};
    } else {
        auto itr = collections.find(cid);
        if (itr != collections.end()) {
            return itr->second.sid;
        }
    }
    return {};
}

void Manifest::dump() const {
    std::cerr << *this << std::endl;
}

bool CollectionEntry::operator==(const CollectionEntry& other) const {
    return id == other.id && maxTtl == other.maxTtl;
}

bool Scope::operator==(const Scope& other) const {
    bool equal = name == other.name &&
                 collections.size() == other.collections.size();
    if (equal) {
        for (const auto c : collections) {
            equal &= std::find(other.collections.begin(),
                               other.collections.end(),
                               c) != other.collections.end();
            if (!equal) {
                break;
            }
        }
    }
    return equal;
}

bool Manifest::Collection::operator==(const Manifest::Collection& other) const {
    return sid == other.sid && name == other.name;
}

bool Manifest::operator==(const Manifest& other) const {
    bool equal = defaultCollectionExists == other.defaultCollectionExists;
    equal &= uid == other.uid;
    equal &= scopes == other.scopes && collections == other.collections;
    return equal;
}

cb::engine_error Manifest::isSuccessor(const Manifest& successor) const {
    // if forced return true - anything can happen
    // else must be a > uid with sane changes or equal uid and no changes
    if (successor.getUid() > uid) {
        // For each scope-id in this is it in successor?
        for (const auto& [sid, scope] : scopes) {
            auto itr = successor.findScope(sid);
            // If the sid still exists it must have the same name
            if (itr != successor.endScopes()) {
                if (scope.name != itr->second.name) {
                    return cb::engine_error(
                            cb::engine_errc::cannot_apply_collections_manifest,
                            "invalid name change detected on scope "
                            "sid:" + sid.to_string() +
                                    ", name:" + scope.name +
                                    ", new-name:" + itr->second.name);
                }
            } // else this sid has been removed and that's fine
        }

        // For each collection in this is it in successor?
        for (const auto& [cid, collection] : collections) {
            auto itr = successor.findCollection(cid);
            if (itr != successor.end()) {
                // Name and scope-id must be equal
                if (collection != itr->second) {
                    return cb::engine_error(
                            cb::engine_errc::cannot_apply_collections_manifest,
                            "invalid collection change detected "
                            "cid:" + cid.to_string() +
                                    ", name:" + collection.name +
                                    ", sid:" + collection.sid.to_string() +
                                    ", new-name:" + itr->second.name +
                                    ", new-sid: " +
                                    itr->second.sid.to_string());
                }
            } // else this cid has been removed and that's fine
        }
    } else if (uid == successor.getUid()) {
        if (*this != successor) {
            return cb::engine_error(
                    cb::engine_errc::cannot_apply_collections_manifest,
                    "equal uid but not an equal manifest");
        }
    } else {
        return cb::engine_error(
                cb::engine_errc::cannot_apply_collections_manifest,
                "uid must be >= current-uid:" + std::to_string(uid) +
                        ", new-uid:" + std::to_string(successor.getUid()));
    }

    return cb::engine_error(cb::engine_errc::success, "");
}

bool Manifest::isEpoch() const {
    // uid of 0, 1 scope and 1 collection
    if (uid == 0 && scopes.size() == 1 && collections.size() == 1) {
        const auto scope = findScope(ScopeID::Default);
        return defaultCollectionExists && scope != scopes.end() &&
               scope->second.name == DefaultScopeIdentifier;
    }
    return false;
}

std::ostream& operator<<(std::ostream& os, const Manifest& manifest) {
    os << "Collections::Manifest"
       << ", defaultCollectionExists:" << manifest.defaultCollectionExists
       << ", uid:" << manifest.uid
       << ", collections.size:" << manifest.collections.size() << std::endl;
    for (const auto& entry : manifest.scopes) {
        os << "scope:{" << std::hex << entry.first << ", " << entry.second.name
           << ", collections:[";
        for (const auto& collection : entry.second.collections) {
            os << "{" << std::hex << collection.id << ", sid:" << std::hex
               << manifest.collections.at(collection.id).sid << ", "
               << manifest.collections.at(collection.id).name << "}";
        }
        os << "]\n";
    }
    return os;
}
}
