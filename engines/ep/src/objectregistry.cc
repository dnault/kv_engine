/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2015 Couchbase, Inc
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

#include "objectregistry.h"

#include "ep_engine.h"
#include "item.h"
#include "stored-value.h"
#include "threadlocal.h"
#include <platform/cb_arena_malloc.h>

#if 1
static ThreadLocal<EventuallyPersistentEngine*> *th;

/**
 * Object registry link hook for getting the registry thread local
 * installed.
 */
class installer {
public:
   installer() {
      if (th == NULL) {
         th = new ThreadLocal<EventuallyPersistentEngine*>();
      }
   }

   ~installer() {
       delete th;
   }
} install;

static bool verifyEngine(EventuallyPersistentEngine *engine)
{
   if (engine == nullptr) {
       static const char* allowNoStatsUpdate = getenv("ALLOW_NO_STATS_UPDATE");
       if (allowNoStatsUpdate) {
           return false;
       } else {
           throw std::logic_error("verifyEngine: engine should be non-NULL");
       }
   }
   return true;
}

void ObjectRegistry::onCreateBlob(const Blob *blob)
{
   EventuallyPersistentEngine *engine = th->get();
   if (verifyEngine(engine)) {
       auto& coreLocalStats = engine->getEpStats().coreLocal.get();

       size_t size = cb::ArenaMalloc::malloc_usable_size(blob);
       coreLocalStats->blobOverhead.fetch_add(size - blob->getSize());
       coreLocalStats->currentSize.fetch_add(size);
       coreLocalStats->totalValueSize.fetch_add(size);
       coreLocalStats->numBlob++;
   }
}

void ObjectRegistry::onDeleteBlob(const Blob *blob)
{
   EventuallyPersistentEngine *engine = th->get();
   if (verifyEngine(engine)) {
       auto& coreLocalStats = engine->getEpStats().coreLocal.get();

       size_t size = cb::ArenaMalloc::malloc_usable_size(blob);
       coreLocalStats->blobOverhead.fetch_sub(size - blob->getSize());
       coreLocalStats->currentSize.fetch_sub(size);
       coreLocalStats->totalValueSize.fetch_sub(size);
       coreLocalStats->numBlob--;
   }
}

void ObjectRegistry::onCreateStoredValue(const StoredValue *sv)
{
   EventuallyPersistentEngine *engine = th->get();
   if (verifyEngine(engine)) {
       auto& coreLocalStats = engine->getEpStats().coreLocal.get();

       size_t size = cb::ArenaMalloc::malloc_usable_size(sv);
       coreLocalStats->numStoredVal++;
       coreLocalStats->totalStoredValSize.fetch_add(size);
   }
}

void ObjectRegistry::onDeleteStoredValue(const StoredValue *sv)
{
   EventuallyPersistentEngine *engine = th->get();
   if (verifyEngine(engine)) {
       auto& coreLocalStats = engine->getEpStats().coreLocal.get();

       size_t size = cb::ArenaMalloc::malloc_usable_size(sv);
       coreLocalStats->totalStoredValSize.fetch_sub(size);
       coreLocalStats->numStoredVal--;
   }
}


void ObjectRegistry::onCreateItem(const Item *pItem)
{
   EventuallyPersistentEngine *engine = th->get();
   if (verifyEngine(engine)) {
       auto& coreLocalStats = engine->getEpStats().coreLocal.get();
       coreLocalStats->memOverhead.fetch_add(pItem->size() -
                                             pItem->getValMemSize());
       ++coreLocalStats->numItem;
   }
}

void ObjectRegistry::onDeleteItem(const Item *pItem)
{
   EventuallyPersistentEngine *engine = th->get();
   if (verifyEngine(engine)) {
       auto& coreLocalStats = engine->getEpStats().coreLocal.get();
       coreLocalStats->memOverhead.fetch_sub(pItem->size() -
                                             pItem->getValMemSize());
       --coreLocalStats->numItem;
   }
}

EventuallyPersistentEngine *ObjectRegistry::getCurrentEngine() {
    return th->get();
}

EventuallyPersistentEngine *ObjectRegistry::onSwitchThread(
                                            EventuallyPersistentEngine *engine,
                                            bool want_old_thread_local) {
    EventuallyPersistentEngine *old_engine = nullptr;

    if (want_old_thread_local) {
        old_engine = th->get();
    }

    // Set the engine so that onDeleteItem etc... can update their stats
    th->set(engine);

    // Next tell ArenaMalloc what todo so that we can account memory to the
    // bucket
    if (engine) {
        cb::ArenaMalloc::switchToClient(engine->getArenaMallocClient());
    } else {
        cb::ArenaMalloc::switchFromClient();
    }
    return old_engine;
}

NonBucketAllocationGuard::NonBucketAllocationGuard()
    : engine(ObjectRegistry::onSwitchThread(nullptr, true)) {
}

NonBucketAllocationGuard::~NonBucketAllocationGuard() {
    ObjectRegistry::onSwitchThread(engine);
}

BucketAllocationGuard::BucketAllocationGuard(EventuallyPersistentEngine* engine)
    : previous(ObjectRegistry::onSwitchThread(engine, true)) {
}

BucketAllocationGuard::~BucketAllocationGuard() {
    ObjectRegistry::onSwitchThread(previous);
}

#endif
