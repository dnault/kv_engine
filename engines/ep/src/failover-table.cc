/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2014 Couchbase, Inc
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
#include <nlohmann/json.hpp>
#include <platform/checked_snprintf.h>

#include "atomic.h"
#include "bucket_logger.h"
#include "failover-table.h"
#include "statistics/collector.h"

FailoverTable::FailoverTable(size_t capacity)
    : max_entries(capacity), erroneousEntriesErased(0) {
    createEntry(0);
    cacheTableJSON();
}

FailoverTable::FailoverTable(const std::string& json,
                             size_t capacity,
                             int64_t highSeqno)
    : max_entries(capacity), erroneousEntriesErased(0) {
    if (!loadFromJSON(json)) {
        throw std::invalid_argument("FailoverTable(): unable to load from "
                "JSON file '" + json + "'");
    }
    sanitizeFailoverTable(highSeqno);
}

FailoverTable::~FailoverTable() { }

failover_entry_t FailoverTable::getLatestEntry() const {
    std::lock_guard<std::mutex> lh(lock);
    return table.front();
}

void FailoverTable::removeLatestEntry() {
    std::lock_guard<std::mutex> lh(lock);
    if (!table.empty()) {
        table.pop_front();
        cacheTableJSON();
    }
}

uint64_t FailoverTable::getLatestUUID() const {
    return latest_uuid.load();
}

void FailoverTable::createEntry(uint64_t high_seqno) {
    std::lock_guard<std::mutex> lh(lock);

    // Our failover table represents only *our* branch of history.
    // We must remove branches we've diverged from.
    // Entries that we remove here are not erroneous entries because a
    // diverged branch due to node(s) failure(s).
    table.remove_if([high_seqno](const failover_entry_t& e) {
                        return (e.by_seqno > high_seqno);
                    });

    failover_entry_t entry;
    /* In past we have seen some erroneous entries in failover table with
       vb_uuid == 0 due to some bugs in the code which read/wrote the failover
       table from/to the disk or due to a some unknown buggy code.
       Hence we choose not to have 0 as a valid vb_uuid value. Loop below
       regenerates the vb_uuid in case 0 is generated by random generator */
    do {
        entry.vb_uuid = (provider.next() >> 16);
    } while(0 == entry.vb_uuid);
    entry.by_seqno = high_seqno;
    table.push_front(entry);
    latest_uuid = entry.vb_uuid;

    // Cap the size of the table
    while (table.size() > max_entries) {
        table.pop_back();
    }
    cacheTableJSON();
}

bool FailoverTable::getLastSeqnoForUUID(uint64_t uuid,
                                        uint64_t *seqno) const {
    std::lock_guard<std::mutex> lh(lock);
    auto curr_itr = table.begin();
    table_t::const_iterator prev_itr;

    if (curr_itr->vb_uuid == uuid) {
        return false;
    }

    prev_itr = curr_itr;

    ++curr_itr;

    for (; curr_itr != table.end(); ++curr_itr) {
        if (curr_itr->vb_uuid == uuid) {
            *seqno = prev_itr->by_seqno;
            return true;
        }

        prev_itr = curr_itr;
    }

    return false;
}

std::pair<bool, std::string> FailoverTable::needsRollback(
        uint64_t start_seqno,
        uint64_t cur_seqno,
        uint64_t vb_uuid,
        uint64_t snap_start_seqno,
        uint64_t snap_end_seqno,
        uint64_t purge_seqno,
        bool strictVbUuidMatch,
        std::optional<uint64_t> maxCollectionHighSeqno,
        uint64_t* rollback_seqno) const {
    /* Start with upper as vb highSeqno */
    uint64_t upper = cur_seqno;
    std::lock_guard<std::mutex> lh(lock);

    /* Clients can have a diverging (w.r.t producer) branch at seqno 0 and in
       such a case, some of them strictly need a rollback and others don't.
       So we should NOT rollback when a client has a vb_uuid == 0 or
       if does not expect a rollback at start_seqno == 0 */
    if (start_seqno == 0 && (!strictVbUuidMatch || vb_uuid == 0)) {
        return std::make_pair(false, std::string());
    }

    *rollback_seqno = 0;

    /* One of the reasons for rollback is client being in middle of a snapshot.
       We compare snapshot_start and snapshot_end with start_seqno to see if
       the client is really in the middle of a snapshot. To prevent unnecessary
       rollback, we update snap_start_seqno/snap_end_seqno accordingly and then
       use those values for rollback calculations below */
    adjustSnapshotRange(start_seqno, snap_start_seqno, snap_end_seqno);

    /*
     * If this request is for a collection stream then check if we can really
     * need to roll the client back if the start_seqno < purge_seqno.
     * We should allow the request if the start_seqno indicates that the client
     * has all mutations/events for the collections the stream is for.
     */
    bool allowNonRollBackCollectionStream = false;
    if (maxCollectionHighSeqno.has_value()) {
        allowNonRollBackCollectionStream =
                start_seqno < purge_seqno &&
                start_seqno >= maxCollectionHighSeqno.value() &&
                maxCollectionHighSeqno.value() <= purge_seqno;
    }

    /* There may be items that are purged during compaction. We need
       to rollback to seq no 0 in that case, only if we have purged beyond
       start_seqno and if start_seqno is not 0 */
    if (start_seqno < purge_seqno && start_seqno != 0 &&
        !allowNonRollBackCollectionStream) {
        return std::make_pair(true,
                              std::string("purge seqno (") +
                                      std::to_string(purge_seqno) +
                                      ") is greater than start seqno - "
                                      "could miss purged deletions");
    }

    table_t::const_reverse_iterator itr;
    for (itr = table.rbegin(); itr != table.rend(); ++itr) {
        if (itr->vb_uuid == vb_uuid) {
            if (++itr != table.rend()) {
                /* Since producer has more history we need to consider the
                   next seqno in failover table as upper */
                upper = itr->by_seqno;
            }
            --itr; /* Get back the iterator to current entry */
            break;
        }
    }

    /* Find the rollback point */
    if (itr == table.rend()) {
        /* No vb_uuid match found in failover table, so producer and consumer
         have no common history. Rollback to zero */
        return std::make_pair(
                true,
                std::string("vBucket UUID not found in failover table, "
                            "consumer and producer have no common history"));
    } else {
        if (snap_end_seqno <= upper) {
            /* No rollback needed as producer and consumer histories are same */
            return {false, ""};
        } else {
            /* We need a rollback as producer upper is lower than the end in
               consumer snapshot */
            if (upper < snap_start_seqno) {
                *rollback_seqno = upper;
            } else {
                /* We have to rollback till snap_start_seqno to handle
                   deduplication case */
                *rollback_seqno = snap_start_seqno;
            }
            return std::make_pair(
                    true,
                    std::string(
                            "consumer ahead of producer - producer upper at ") +
                            std::to_string(upper));
        }
    }
}

void FailoverTable::pruneEntries(uint64_t seqno) {
    // Not permitted to remove the initial table entry (i.e. seqno zero).
    if (seqno == 0) {
        throw std::invalid_argument("FailoverTable::pruneEntries: "
                                    "cannot prune entry zero");
    }
    std::lock_guard<std::mutex> lh(lock);

    auto seqno_too_high = [seqno](failover_entry_t& entry) {
        return entry.by_seqno > seqno;
    };

    // Count how many this would remove
    auto count = std::count_if(table.begin(), table.end(), seqno_too_high);
    if (table.size() - count < 1) {
        throw std::invalid_argument("FailoverTable::pruneEntries: cannot "
                "prune up to seqno " + std::to_string(seqno) +
                " as it would result in less than one element in failover table");
    }

    // Preconditions look good; remove them.
    table.remove_if(seqno_too_high);

    latest_uuid = table.front().vb_uuid;

    cacheTableJSON();
}

std::string FailoverTable::toJSON() {
    std::lock_guard<std::mutex> lh(lock);
    // Here we are explictly forcing a copy of the object to
    // work around std::string copy-on-write data-race issues
    // seen on some versions of libstdc++ - see MB-18510
    return std::string(cachedTableJSON.begin(), cachedTableJSON.end());
}

void FailoverTable::cacheTableJSON() {
    nlohmann::json json = nlohmann::json::array();
    table_t::iterator it;
    for (it = table.begin(); it != table.end(); ++it) {
        nlohmann::json obj;
        obj["id"] = (*it).vb_uuid;
        obj["seq"] = (*it).by_seqno;
        json.push_back(obj);
    }
    cachedTableJSON = json.dump();
}

void FailoverTable::addStats(const void* cookie,
                             Vbid vbid,
                             const AddStatFn& add_stat) {
    std::lock_guard<std::mutex> lh(lock);
    try {
        char statname[80] = {0};
        checked_snprintf(
                statname, sizeof(statname), "vb_%d:num_entries", vbid.get());
        add_casted_stat(statname, table.size(), add_stat, cookie);
        checked_snprintf(statname,
                         sizeof(statname),
                         "vb_%d:num_erroneous_entries_erased",
                         vbid.get());
        add_casted_stat(statname, getNumErroneousEntriesErased(), add_stat,
                        cookie);

        table_t::iterator it;
        int entrycounter = 0;
        for (it = table.begin(); it != table.end(); ++it) {
            checked_snprintf(statname,
                             sizeof(statname),
                             "vb_%d:%d:id",
                             vbid.get(),
                             entrycounter);
            add_casted_stat(statname, it->vb_uuid, add_stat, cookie);
            checked_snprintf(statname,
                             sizeof(statname),
                             "vb_%d:%d:seq",
                             vbid.get(),
                             entrycounter);
            add_casted_stat(statname, it->by_seqno, add_stat, cookie);
            entrycounter++;
        }
    } catch (std::exception& error) {
        EP_LOG_WARN("FailoverTable::addStats: Failed to build stats: {}",
                    error.what());
    }
}

std::vector<vbucket_failover_t> FailoverTable::getFailoverLog() {
    std::lock_guard<std::mutex> lh(lock);
    std::vector<vbucket_failover_t> result;
    for (const auto& entry : table) {
        vbucket_failover_t failoverEntry;
        failoverEntry.uuid = entry.vb_uuid;
        failoverEntry.seqno = entry.by_seqno;
        result.push_back(failoverEntry);
    }
    return result;
}

bool FailoverTable::loadFromJSON(const nlohmann::json& json) {
    if (!json.is_array()) {
        return false;
    }

    table_t new_table;

    for (const auto& it : json) {
        if (!it.is_object()) {
            return false;
        }

        auto jid = it.find("id");
        auto jseq = it.find("seq");

        if (jid == it.end() || !jid->is_number()) {
            return false;
        }
        if (jseq == it.end() || !jseq->is_number()) {
            return false;
        }

        failover_entry_t entry;
        entry.vb_uuid = *jid;
        entry.by_seqno = *jseq;
        new_table.push_back(entry);
    }

    // Must have at least one element in the failover table.
    if (new_table.empty()) {
        return false;
    }

    table = new_table;
    latest_uuid = table.front().vb_uuid;

    return true;
}

bool FailoverTable::loadFromJSON(const std::string& json) {
    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(json);
    } catch (const nlohmann::json::exception& e) {
        EP_LOG_WARN(
                "FailoverTable::loadFromJSON: Failed to parse JSON string: {}",
                e.what());
        return false;
    }

    auto ret = loadFromJSON(parsed);
    cachedTableJSON = json;

    return ret;
}

void FailoverTable::replaceFailoverLog(const uint8_t* bytes, uint32_t length) {
    std::lock_guard<std::mutex> lh(lock);
    if ((length % 16) != 0 || length == 0) {
        throw std::invalid_argument("FailoverTable::replaceFailoverLog: "
                "length (which is " + std::to_string(length) +
                ") must be a non-zero multiple of 16");
    }
    table.clear();

    for (; length > 0; length -=16) {
        failover_entry_t entry;
        memcpy(&entry.by_seqno, bytes + length - 8, sizeof(uint64_t));
        memcpy(&entry.vb_uuid, bytes + length - 16, sizeof(uint64_t));
        entry.by_seqno = ntohll(entry.by_seqno);
        entry.vb_uuid = ntohll(entry.vb_uuid);
        table.push_front(entry);
    }

    latest_uuid = table.front().vb_uuid;

    cacheTableJSON();
}

size_t FailoverTable::getNumEntries() const
{
    return table.size();
}

void FailoverTable::adjustSnapshotRange(uint64_t start_seqno,
                                        uint64_t &snap_start_seqno,
                                        uint64_t &snap_end_seqno)
{
    if (start_seqno == snap_end_seqno) {
        /* Client already has all elements in the snapshot */
        snap_start_seqno = start_seqno;
    } else if (start_seqno == snap_start_seqno) {
        /* Client has no elements in the snapshot */
        snap_end_seqno = start_seqno;
    }
}

void FailoverTable::sanitizeFailoverTable(int64_t highSeqno) {
    size_t intialTableSize = table.size();
    for (auto itr = table.begin(); itr != table.end(); ) {
        if (0 == itr->vb_uuid) {
            /* 1. Prune entries with vb_uuid == 0. (From past experience we have
                  seen erroneous entries mostly have vb_uuid == 0, hence we have
                  chosen not to use 0 as valid vb_uuid) */
            itr = table.erase(itr);
            continue;
        }
        if (itr != table.begin()) {
            auto prevItr = std::prev(itr);
            if (itr->by_seqno > prevItr->by_seqno) {
                /* 2. Prune any entry that has a by_seqno greater than by_seqno
                      of prev entry. (Entries are pushed at the head of the
                      table and must have seqno > seqno of following entries) */
                itr = table.erase(itr);
                continue;
            }
        }
        ++itr;
    }
    erroneousEntriesErased += (intialTableSize - table.size());

    if (table.empty()) {
        createEntry(highSeqno);
    } else if (erroneousEntriesErased) {
        cacheTableJSON();
    }
}

size_t FailoverTable::getNumErroneousEntriesErased() const {
    return erroneousEntriesErased;
}

std::ostream& operator<<(std::ostream& os, const failover_entry_t& entry) {
    os << R"({"vb_uuid":")" << entry.vb_uuid << R"(", "by_seqno":")"
       << entry.by_seqno << "\"}";
    return os;
}

std::ostream& operator<<(std::ostream& os, const FailoverTable& table) {
    std::lock_guard<std::mutex> lh(table.lock);
    os << "FailoverTable: max_entries:" << table.max_entries
       << ", erroneousEntriesErased:" << table.erroneousEntriesErased
       << ", latest_uuid:" << table.latest_uuid << "\n";
    os << "  cachedTableJSON:" << table.cachedTableJSON << "\n";
    os << "  table: {\n";
    for (const auto& e : table.table) {
        os << "    " << e << "\n";
    }
    os << "  }";

    return os;
}
