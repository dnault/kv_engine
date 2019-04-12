/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2019 Couchbase, Inc.
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
#pragma once

#include "durability_monitor.h"

#include "ep_types.h"
#include "monotonic.h"

#include "memcached/durability_spec.h"
#include "memcached/engine_common.h"
#include "memcached/engine_error.h"

#include <folly/Synchronized.h>
#include <nlohmann/json.hpp>

#include <list>
#include <unordered_set>

class StoredDocKey;
class StoredValue;
class VBucket;

/*
 * The DurabilityMonitor (DM) drives the finalization (commit/abort) of a
 * SyncWrite request.
 *
 * To do that, the DM tracks the pending SyncWrites and the replica
 * acknowledgements to verify if the Durability Requirement is satisfied for
 * the tracked mutations.
 *
 * DM internals (describing by example).
 *
 * @todo: Update deferred, internals changing frequently these days :)
 */
class ActiveDurabilityMonitor : public DurabilityMonitor {
public:
    // Note: constructor and destructor implementation in the .cc file to allow
    // the forward declaration of ReplicationChain in the header
    ActiveDurabilityMonitor(VBucket& vb);
    ~ActiveDurabilityMonitor();

    /**
     * Sets the Replication Topology.
     *
     * @param topology The topology encoded as nlohmann::json array of (max 2)
     *     replication chains. Each replication chain is itself a
     *     nlohmann::json array of nodes representing the chain.
     * @throw std::invalid_argument
     */
    void setReplicationTopology(const nlohmann::json& topology);

    /**
     * @return the replication topology
     */
    const nlohmann::json& getReplicationTopology() const;

    /// @returns the high_prepared_seqno.
    int64_t getHighPreparedSeqno() const override;

    /**
     * @return true if the replication topology allows Majority being reached,
     *     false otherwise
     */
    bool isDurabilityPossible() const;

    /**
     * Start tracking a new SyncWrite.
     * Expected to be called by VBucket::add/update/delete after a new SyncWrite
     * has been inserted into the HashTable and enqueued into the
     * CheckpointManager.
     *
     * @param cookie Optional client cookie which will be notified the SyncWrite
     *        completes.
     * @param item the queued_item
     * @throw std::logic_error if the replication-chain is not set
     */
    void addSyncWrite(const void* cookie, queued_item item);

    /**
     * Expected to be called by memcached at receiving a DCP_SEQNO_ACK packet.
     *
     * @param replica The replica that sent the ACK
     * @param diskSeqno The ack'ed prepared seqno.
     * @return ENGINE_SUCCESS if the operation succeeds, an error code otherwise
     * @throw std::logic_error if the received seqno is unexpected
     */
    ENGINE_ERROR_CODE seqnoAckReceived(const std::string& replica,
                                       int64_t preparedSeqno);

    /**
     * Enforce timeout for the expired SyncWrites in the tracked list.
     *
     * @param asOf The time to be compared with tracked-SWs' expiry-time
     * @throw std::logic_error
     */
    void processTimeout(std::chrono::steady_clock::time_point asOf);

    /**
     * Advances the local disk-tracking to the last persisted seqno for VBucket.
     * Expected to be called by the Flusher.
     *
     * @throw std::logic_error if the replication-chain is not set
     */
    void notifyLocalPersistence();

    /**
     * Output DurabiltyMonitor stats.
     *
     * @param addStat the callback to memcached
     * @param cookie
     */
    void addStats(const AddStatFn& addStat, const void* cookie) const override;

protected:
    class SyncWrite;
    struct ReplicationChain;
    struct Position;
    struct NodePosition;

    using Container = std::list<SyncWrite>;

    size_t getNumTracked() const override;

    void toOStream(std::ostream& os) const override;

    /**
     * @return the size of FirstChain
     */
    uint8_t getFirstChainSize() const;

    /**
     * @return the FirstChain Majority
     */
    uint8_t getFirstChainMajority() const;

    /**
     * Returns the seqnos of the SyncWrites currently pointed by the
     * internal memory/disk tracking for Node. E.g., if we have a tracked
     * SyncWrite list like {s:1, s:2} and we receive a SeqnoAck{mem:2,
     * disk:1}, then the internal memory/disk tracking will be {mem:2,
     * disk:1}, which is what this function returns. Note that this may
     * differ from Replica AckSeqno. Using the same example, if we receive a
     * SeqnoAck{mem:100, disk:100} then the internal tracking will still
     * point to {mem:2, disk:1}, which is what this function will return
     * again.
     *
     * @param node
     * @return the {memory, disk} seqnos of the tracked writes for node
     */
    NodeSeqnos getNodeWriteSeqnos(const std::string& node) const;

    /**
     * Returns the last {memSeqno, diskSeqno} ack'ed by Node.
     * Note that this may differ from Node WriteSeqno.
     *
     * @param node
     * @return the last {memory, disk} seqnos ack'ed by Node
     */
    NodeSeqnos getNodeAckSeqnos(const std::string& node) const;

    /**
     * Commit the given SyncWrite.
     *
     * @param sw The SyncWrite to commit
     */
    void commit(const SyncWrite& sw);

    /**
     * Abort the given SyncWrite.
     *
     * @param sw The SyncWrite to abort
     */
    void abort(const SyncWrite& sw);

    /**
     * Test only.
     *
     * @return the set of seqnos tracked by this DurabilityMonitor
     */
    std::unordered_set<int64_t> getTrackedSeqnos() const;

    /**
     * Test only (for now; shortly this will be probably needed at rollback).
     * Removes all SyncWrites from the tracked container. Replication chain
     * iterators stay valid.
     *
     * @returns the number of SyncWrites removed from tracking
     */
    size_t wipeTracked();

    /*
     * This class embeds the state of an ADM. It has been designed for being
     * wrapped by a folly::Synchronized<T>, which manages the read/write
     * concurrent access to the T instance.
     */
    struct State {
        void setReplicationTopology(const nlohmann::json& topology);

        void addSyncWrite(const void* cookie, const queued_item& item);

        /**
         * Returns the next position for a node iterator.
         *
         * @param node
         * @param tracking Memory or Disk?
         * @return the iterator to the next position for the given node
         */
        Container::iterator getNodeNext(const std::string& node,
                                        Tracking tracking);

        /**
         * Advance a node tracking to the next Position in the tracked
         * Container. Note that a Position tracks a node in terms of both:
         * - iterator to a SyncWrite in the tracked Container
         * - seqno of the last SyncWrite ack'ed by the node
         * This function advances both iterator and seqno.
         *
         * @param node
         * @param tracking Memory or Disk?
         */
        void advanceNodePosition(const std::string& node, Tracking tracking);

        /**
         * We track both the memory/disk seqnos ack'ed by nodes.
         * Note that this may be different from the current SyncWrite tracked
         * for the node. E.g., if we have one tracked SyncWrite{seqno:1,
         * Level:Majority}, then the DurabilityMonitor may receive a
         * SeqnoAck{mem:1000, disk:0}. At that point the memory-tracking for
         * that node will be:
         *
         *     {writeSeqno:1, ackSeqno:1000}
         *
         * This function updates the tracking with the last seqno ack'ed by
         * node.
         *
         * @param node
         * @param tracking Memory or Disk?
         * @param seqno New ack seqno
         */
        void updateNodeAck(const std::string& node,
                           Tracking tracking,
                           int64_t seqno);

        /**
         * Updates a node memory/disk tracking as driven by the new ack-seqno.
         *
         * @param node The node that ack'ed the given seqno
         * @param tracking Memory or Disk?
         * @param ackSeqno
         * @param [out] toCommit
         */
        void processSeqnoAck(const std::string& node,
                             Tracking tracking,
                             int64_t ackSeqno,
                             Container& toCommit);

        /**
         * Removes all the expired Prepares from tracking.
         *
         * @param asOf The time to be compared with tracked-SWs' expiry-time
         * @param [out] the list of the expired Prepares
         */
        void removeExpired(std::chrono::steady_clock::time_point asOf,
                           Container& expired);

        void addStats(const AddStatFn& addStat,
                      const void* cookie,
                      uint16_t vbid) const;

        const std::string& getActive() const;

        NodeSeqnos getNodeWriteSeqnos(const std::string& node) const;

        NodeSeqnos getNodeAckSeqnos(const std::string& node) const;

        /**
         * Remove the given SyncWrte from tracking.
         *
         * @param it The iterator to the SyncWrite to be removed
         * @return single-element list of the removed SyncWrite.
         */
        Container removeSyncWrite(Container::iterator it);

        /// The container of pending Prepares.
        Container trackedWrites;
        // @todo: Expand for supporting the SecondChain.
        std::unique_ptr<ReplicationChain> firstChain;
        // Always stores the seqno of the last SyncWrite added for tracking.
        // Useful for sanity checks, necessary because the tracked container
        // can by emptied by Commit/Abort.
        Monotonic<int64_t, ThrowExceptionPolicy> lastTrackedSeqno;
    };

    // The VBucket owning this DurabilityMonitor instance
    VBucket& vb;

    folly::Synchronized<State> state;

    const size_t maxReplicas = 3;

    friend std::ostream& operator<<(std::ostream& os,
                                    const ActiveDurabilityMonitor& dm);
    friend std::ostream& operator<<(std::ostream&,
                                    const ActiveDurabilityMonitor::SyncWrite&);
};

std::ostream& operator<<(std::ostream& os, const ActiveDurabilityMonitor& dm);