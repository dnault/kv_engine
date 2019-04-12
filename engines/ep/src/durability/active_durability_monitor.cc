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

#include "active_durability_monitor.h"

#include "bucket_logger.h"
#include "item.h"
#include "statwriter.h"
#include "stored-value.h"
#include "vbucket.h"

#include <gsl.h>
#include <unordered_map>

// An empty string is used to indicate an undefined node in a replication
// topology.
static const std::string UndefinedNode{};

/*
 * Represents the tracked state of a replica.
 * Note that the lifetime of a Position is determined by the logic in
 * DurabilityMonitor.
 *
 * - it: Iterator that points to a position in the Container of tracked
 *         SyncWrites. This is an optimization: logically it points always to
 *         the last SyncWrite acknowledged by the tracked replica, so that we
 *         can avoid any O(N) scan when updating the replica state at seqno-ack
 *         received. It may point to Container::end (e.g, when the pointed
 *         SyncWrite is the last element in Container and it is removed).
 *
 * - lastWriteSeqno: Stores always the seqno of the last SyncWrite
 *         acknowledged by the tracked replica, even when Position::it points to
 *         Container::end. Used for validation at seqno-ack received and stats.
 *
 * - lastAckSeqno: Stores always the last seqno acknowledged by the tracked
 *         replica. Used for validation at seqno-ack received and stats.
 */
struct ActiveDurabilityMonitor::Position {
    Position(const Container::iterator& it) : it(it) {
    }
    Container::iterator it;
    WeaklyMonotonic<int64_t, ThrowExceptionPolicy> lastWriteSeqno{0};
    WeaklyMonotonic<int64_t, ThrowExceptionPolicy> lastAckSeqno{0};
};

struct ActiveDurabilityMonitor::NodePosition {
    Position memory;
    Position disk;
};

/*
 * Represents a VBucket Replication Chain in the ns_server meaning,
 * i.e. a list of active/replica nodes where the VBucket resides.
 */
struct ActiveDurabilityMonitor::ReplicationChain {
    /**
     * @param nodes The list of active/replica nodes in the ns_server format:
     *     {active, replica1, replica2, replica3}
     *
     * replica node(s) (but not active) can be logically undefined - if:
     * a) auto-failover has occurred but the cluster hasn't yet been rebalanced
     *    - as such the old replica (which is now the active) hasn't been
     *    replaced yet.
     * b) Bucket has had the replica count increased but not yet reblanced
     *    (to assign the correct replicas. An undefined replica is represented
     *    by an empty node name (""s).
     */
    ReplicationChain(const std::vector<std::string>& nodes,
                     const Container::iterator& it)
        : majority(nodes.size() / 2 + 1), active(nodes.at(0)) {
        if (nodes.at(0) == UndefinedNode) {
            throw std::invalid_argument(
                    "ReplicationChain::ReplicationChain: Active node cannot be "
                    "undefined");
        }

        for (const auto& node : nodes) {
            if (node == UndefinedNode) {
                // unassigned, don't register a position in the chain.
                continue;
            }
            // This check ensures that there is no duplicate in the given chain
            if (!positions
                         .emplace(node,
                                  NodePosition{Position(it), Position(it)})
                         .second) {
                throw std::invalid_argument(
                        "ReplicationChain::ReplicationChain: Duplicate node: " +
                        node);
            }
        }
    }

    size_t size() const {
        return positions.size();
    }

    bool isDurabilityPossible() const {
        Expects(size());
        Expects(majority);
        return size() >= majority;
    }

    // Index of node Positions. The key is the node id.
    // A Position embeds the seqno-state of the tracked node.
    std::unordered_map<std::string, NodePosition> positions;

    // Majority in the arithmetic definition:
    //     chain-size / 2 + 1
    const uint8_t majority;

    const std::string active;
};

/*
 * Represents a tracked SyncWrite.
 */
class ActiveDurabilityMonitor::SyncWrite {
public:
    SyncWrite(const void* cookie,
              queued_item item,
              const ReplicationChain& chain)
        : cookie(cookie),
          item(item),
          majority(chain.majority),
          expiryTime(
                  item->getDurabilityReqs().getTimeout()
                          ? std::chrono::steady_clock::now() +
                                    std::chrono::milliseconds(
                                            item->getDurabilityReqs()
                                                    .getTimeout())
                          : boost::optional<
                                    std::chrono::steady_clock::time_point>{}),
          active(chain.active) {
        // We are making a SyncWrite for tracking, we must have already ensured
        // that the Durability Requirements can be met at this point.
        Expects(chain.size() >= majority);
        for (const auto& entry : chain.positions) {
            acks[entry.first] = Ack();
        }
    }

    const StoredDocKey& getKey() const {
        return item->getKey();
    }

    int64_t getBySeqno() const {
        return item->getBySeqno();
    }

    cb::durability::Requirements getDurabilityReqs() const {
        return item->getDurabilityReqs();
    }

    const void* getCookie() const {
        return cookie;
    }

    /**
     * Notify this SyncWrite that it has been ack'ed by node.
     *
     * @param node
     * @param tracking Memory or Disk ack?
     */
    void ack(const std::string& node, Tracking tracking) {
        if (acks.find(node) == acks.end()) {
            throw std::logic_error("SyncWrite::ack: Node not valid: " + node);
        }

        auto& flag = (tracking == Tracking::Memory ? acks.at(node).memory
                                                   : acks.at(node).disk);
        if (flag) {
            throw std::logic_error("SyncWrite::ack: ACK duplicate for node: " +
                                   node + ", tracking:" + to_string(tracking));
        }
        flag = true;

        auto& count = (tracking == Tracking::Memory ? ackCount.memory
                                                    : ackCount.disk);
        count++;
    }

    /**
     * @return true if the Durability Requirements are satisfied for this
     *     SyncWrite, false otherwise
     */
    bool isSatisfied() const {
        bool ret{false};

        switch (getDurabilityReqs().getLevel()) {
        case cb::durability::Level::Majority:
            ret = ackCount.memory >= majority;
            break;
        case cb::durability::Level::MajorityAndPersistOnMaster:
            ret = ackCount.memory >= majority && acks.at(active).disk;
            break;
        case cb::durability::Level::None:
            throw std::logic_error("SyncWrite::isVerified: Level::None");
        case cb::durability::Level::PersistToMajority:
            ret = ackCount.disk >= majority;
            break;
        }

        return ret;
    }

    /**
     * Check if this SyncWrite is expired or not.
     *
     * @param asOf The time to be compared with this SW's expiry-time
     * @return true if this SW's expiry-time < asOf, false otherwise
     */
    bool isExpired(std::chrono::steady_clock::time_point asOf) const {
        if (!expiryTime) {
            return false;
        }
        return expiryTime < asOf;
    }

private:
    // Client cookie associated with this SyncWrite request, to be notified
    // when the SyncWrite completes.
    const void* cookie;

    // An Item stores all the info that the DurabilityMonitor needs:
    // - seqno
    // - Durability Requirements
    // Note that queued_item is a ref-counted object, so the copy in the
    // CheckpointManager can be safely removed.
    const queued_item item;

    struct Ack {
        bool memory{false};
        bool disk{false};
    };

    // Keeps track of node acks. Entry: <node, ack>
    std::unordered_map<std::string, Ack> acks;

    // This optimization eliminates the need of scanning the ACK map for
    // verifying Durability Requirements
    struct {
        Monotonic<uint8_t> memory{0};
        Monotonic<uint8_t> disk{0};
    } ackCount;

    // Majority in the arithmetic definition: num-nodes / 2 + 1
    const uint8_t majority;

    // Used for enforcing the Durability Requirements Timeout. It is set when
    // this SyncWrite is added for tracking into the DurabilityMonitor.
    const boost::optional<std::chrono::steady_clock::time_point> expiryTime;

    // Name of the active node in replication-chain. Used at Durability
    // Requirements verification.
    const std::string active;

    friend std::ostream& operator<<(std::ostream&, const SyncWrite&);
};

std::ostream& operator<<(std::ostream& os,
                         const ActiveDurabilityMonitor::SyncWrite& sw) {
    os << "SW @" << &sw << " "
       << "cookie:" << sw.cookie << " qi:[key:'" << sw.item->getKey()
       << "' seqno:" << sw.item->getBySeqno()
       << " reqs:" << to_string(sw.item->getDurabilityReqs())
       << "] maj:" << std::to_string(sw.majority)
       << " #ack:[mem:" << std::to_string(sw.ackCount.memory)
       << " disk:" << std::to_string(sw.ackCount.disk) << "] acks:[";
    for (const auto& node : sw.acks) {
        std::string ackNames;
        if (node.second.memory) {
            ackNames += "mem";
        }
        if (node.second.disk) {
            if (node.second.memory) {
                ackNames += ",";
            }
            ackNames += "disk";
        }
        if (!ackNames.empty()) {
            os << node.first << ":" << ackNames << " ";
        }
    }
    os << "]";
    return os;
}

ActiveDurabilityMonitor::ActiveDurabilityMonitor(VBucket& vb) : vb(vb) {
}

ActiveDurabilityMonitor::~ActiveDurabilityMonitor() = default;

void ActiveDurabilityMonitor::setReplicationTopology(
        const nlohmann::json& topology) {
    // @todo: Add support for DurabilityMonitor at Replica
    if (vb.getState() == vbucket_state_t::vbucket_state_replica) {
        throw std::invalid_argument(
                "ActiveDurabilityMonitor::setReplicationTopology: Not "
                "supported at "
                "Replica");
    }

    if (!topology.is_array()) {
        throw std::invalid_argument(
                "ActiveDurabilityMonitor::setReplicationTopology: Topology is "
                "not an "
                "array");
    }

    if (topology.size() == 0) {
        throw std::invalid_argument(
                "ActiveDurabilityMonitor::setReplicationTopology: Topology is "
                "empty");
    }

    const auto& firstChain = topology.at(0);

    if (firstChain.size() == 0) {
        throw std::invalid_argument(
                "ActiveDurabilityMonitor::setReplicationTopology: FirstChain "
                "cannot "
                "be empty");
    }

    // Max Active + MaxReplica
    if (firstChain.size() > 1 + maxReplicas) {
        throw std::invalid_argument(
                "ActiveDurabilityMonitor::setReplicationTopology: Too many "
                "nodes "
                "in chain: " +
                firstChain.dump());
    }

    if (!firstChain.at(0).is_string()) {
        throw std::invalid_argument(
                "ActiveDurabilityMonitor::setReplicationTopology: "
                "first node "
                "in chain (active) cannot be undefined");
    }

    state.wlock()->setReplicationTopology(topology);
}

int64_t ActiveDurabilityMonitor::getHighPreparedSeqno() const {
    // @todo-durability: return a correct value for this.
    return 0;
}

bool ActiveDurabilityMonitor::isDurabilityPossible() const {
    const auto s = state.rlock();
    // @todo: Requirements must be possible for all chains, add check for
    //     SecondChain when it is implemented
    if (!(s->firstChain && s->firstChain->isDurabilityPossible())) {
        return false;
    }
    return true;
}

void ActiveDurabilityMonitor::addSyncWrite(const void* cookie,
                                           queued_item item) {
    auto durReq = item->getDurabilityReqs();

    if (durReq.getLevel() == cb::durability::Level::None) {
        throw std::invalid_argument(
                "ActiveDurabilityMonitor::addSyncWrite: Level::None");
    }

    // The caller must have already checked this and returned a proper error
    // before executing down here. Here we enforce it again for defending from
    // unexpected races between VBucket::setState (which sets the replication
    // topology).
    if (!isDurabilityPossible()) {
        throw std::logic_error(
                "ActiveDurabilityMonitor::addSyncWrite: Impossible");
    }

    auto s = state.wlock();
    s->addSyncWrite(cookie, item);

    // By logic, before this call the item has been enqueued into the
    // CheckpointManager. So, the memory-tracking for the active has implicitly
    // advanced.
    const auto& thisNode = s->getActive();
    s->advanceNodePosition(thisNode, Tracking::Memory);
    s->updateNodeAck(thisNode, Tracking::Memory, item->getBySeqno());

    Ensures(s->getNodeWriteSeqnos(thisNode).memory == item->getBySeqno());
    Ensures(s->getNodeAckSeqnos(thisNode).memory == item->getBySeqno());

    // @todo: Missing step - check for satisfied SyncWrite, we may need to
    //     commit immediately in the no-replica scenario. Consider to do that in
    //     a dedicated function for minimizing contention on front-end threads,
    //     as this function is supposed to execute under VBucket-level lock.
}

ENGINE_ERROR_CODE ActiveDurabilityMonitor::seqnoAckReceived(
        const std::string& replica, int64_t preparedSeqno) {
    // Note:
    // TSan spotted that in the execution path to DM::addSyncWrites we acquire
    // HashBucketLock first and then a lock to DM::state, while here we
    // acquire first the lock to DM::state and then HashBucketLock.
    // This could cause a deadlock by lock inversion (note that the 2 execution
    // paths are expected to execute in 2 different threads).
    // Given that the HashBucketLock here is acquired in the sub-call to
    // VBucket::commit, then to fix I need to release the lock to DM::state
    // before executing DM::commit.
    //
    // By logic the correct order of processing for every verified SyncWrite
    // would be:
    // 1) check if DurabilityRequirements are satisfied
    // 2) if they are, then commit
    // 3) remove the committed SyncWrite from tracking
    //
    // But, we are in the situation where steps 1 and 3 must execute under lock
    // to m, while step 2 must not.
    //
    // For now as quick fix I solve by inverting the order of steps 2 and 3:
    // 1) check if DurabilityRequirements are satisfied
    // 2) if they are, remove the verified SyncWrite from tracking
    // 3) commit the removed (and verified) SyncWrite
    //
    // I don't manage the scenario where step 3 fails yet (note that DM::commit
    // just throws if an error occurs in the current implementation), so this
    // is a @todo.
    Container toCommit;
    {
        auto s = state.wlock();
        // @todo-durability: Now there's just a single prepared_seqno, update
        // seqnoAck processing to only have a single cursor per replica.
        s->processSeqnoAck(replica, Tracking::Memory, preparedSeqno, toCommit);
        s->processSeqnoAck(replica, Tracking::Disk, preparedSeqno, toCommit);
    }

    // Commit the verified SyncWrites
    for (const auto& sw : toCommit) {
        commit(sw);
    }

    return ENGINE_SUCCESS;
}

void ActiveDurabilityMonitor::processTimeout(
        std::chrono::steady_clock::time_point asOf) {
    // @todo: Add support for DurabilityMonitor at Replica
    if (vb.getState() != vbucket_state_active) {
        throw std::logic_error("ActiveDurabilityMonitor::processTimeout: " +
                               vb.getId().to_string() + " state is: " +
                               VBucket::toString(vb.getState()));
    }

    Container toAbort;
    state.wlock()->removeExpired(asOf, toAbort);

    for (const auto& entry : toAbort) {
        abort(entry);
    }
}

void ActiveDurabilityMonitor::notifyLocalPersistence() {
    // We must release the lock to m before calling back to VBucket (in
    // commit()) to avoid a lock inversion with HashBucketLock (same issue as
    // at seqnoAckReceived(), details in there).
    Container toCommit;
    {
        auto s = state.wlock();
        // Note: For the Active, everything up-to last-persisted-seqno is in
        //     consistent state.
        s->processSeqnoAck(s->getActive(),
                           Tracking::Disk,
                           vb.getPersistenceSeqno(),
                           toCommit);
    }

    for (const auto& sw : toCommit) {
        commit(sw);
    }
}

void ActiveDurabilityMonitor::addStats(const AddStatFn& addStat,
                                       const void* cookie) const {
    char buf[256];

    try {
        const auto vbid = vb.getId().get();

        checked_snprintf(buf, sizeof(buf), "vb_%d:state", vbid);
        add_casted_stat(buf, VBucket::toString(vb.getState()), addStat, cookie);

        const auto s = state.rlock();

        checked_snprintf(buf, sizeof(buf), "vb_%d:num_tracked", vbid);
        add_casted_stat(buf, s->trackedWrites.size(), addStat, cookie);

        checked_snprintf(buf, sizeof(buf), "vb_%d:high_prepared_seqno", vbid);
        // @todo: return proper high_prepared_seqno
        add_casted_stat(buf, 0 /*high_prepared_seqno*/, addStat, cookie);

        checked_snprintf(buf, sizeof(buf), "vb_%d:last_tracked_seqno", vbid);
        add_casted_stat(buf, s->lastTrackedSeqno, addStat, cookie);

        checked_snprintf(
                buf, sizeof(buf), "vb_%d:replication_chain_first:size", vbid);
        add_casted_stat(buf,
                        (s->firstChain ? s->firstChain->positions.size() : 0),
                        addStat,
                        cookie);

        if (s->firstChain) {
            for (const auto& entry : s->firstChain->positions) {
                const auto* replica = entry.first.c_str();
                const auto& pos = entry.second;

                checked_snprintf(buf,
                                 sizeof(buf),
                                 "vb_%d:replication_chain_first:%s:memory:last_"
                                 "write_seqno",
                                 vbid,
                                 replica);
                add_casted_stat(
                        buf, pos.memory.lastWriteSeqno, addStat, cookie);
                checked_snprintf(buf,
                                 sizeof(buf),
                                 "vb_%d:replication_chain_first:%s:memory:last_"
                                 "ack_seqno",
                                 vbid,
                                 replica);
                add_casted_stat(buf, pos.memory.lastAckSeqno, addStat, cookie);

                checked_snprintf(buf,
                                 sizeof(buf),
                                 "vb_%d:replication_chain_first:%s:disk:last_"
                                 "write_seqno",
                                 vbid,
                                 replica);
                add_casted_stat(
                        buf, pos.memory.lastWriteSeqno, addStat, cookie);
                checked_snprintf(
                        buf,
                        sizeof(buf),
                        "vb_%d:replication_chain_first:%s:disk:last_ack_seqno",
                        vbid,
                        replica);
                add_casted_stat(buf, pos.disk.lastAckSeqno, addStat, cookie);
            }
        }

    } catch (const std::exception& e) {
        EP_LOG_WARN(
                "ActiveDurabilityMonitor::State:::addStats: error building "
                "stats: {}",
                e.what());
    }
}

size_t ActiveDurabilityMonitor::getNumTracked() const {
    return state.rlock()->trackedWrites.size();
}

uint8_t ActiveDurabilityMonitor::getFirstChainSize() const {
    const auto s = state.rlock();
    return s->firstChain ? s->firstChain->positions.size() : 0;
}

uint8_t ActiveDurabilityMonitor::getFirstChainMajority() const {
    const auto s = state.rlock();
    return s->firstChain ? s->firstChain->majority : 0;
}

ActiveDurabilityMonitor::Container::iterator
ActiveDurabilityMonitor::State::getNodeNext(const std::string& node,
                                            Tracking tracking) {
    const auto& pos = firstChain->positions.at(node);
    const auto& it =
            (tracking == Tracking::Memory ? pos.memory.it : pos.disk.it);
    // Note: Container::end could be the new position when the pointed SyncWrite
    //     is removed from Container and the iterator repositioned.
    //     In that case next=Container::begin
    return (it == trackedWrites.end()) ? trackedWrites.begin() : std::next(it);
}

void ActiveDurabilityMonitor::State::advanceNodePosition(
        const std::string& node, Tracking tracking) {
    const auto& pos_ = firstChain->positions.at(node);
    auto& pos = const_cast<Position&>(tracking == Tracking::Memory ? pos_.memory
                                                                   : pos_.disk);

    if (pos.it == trackedWrites.end()) {
        pos.it = trackedWrites.begin();
    } else {
        pos.it++;
    }

    Expects(pos.it != trackedWrites.end());

    // Note that Position::lastWriteSeqno is always set to the current
    // pointed SyncWrite to keep the replica seqno-state for when the pointed
    // SyncWrite is removed
    pos.lastWriteSeqno = pos.it->getBySeqno();

    // Update the SyncWrite ack-counters, necessary for DurReqs verification
    pos.it->ack(node, tracking);
}

void ActiveDurabilityMonitor::State::updateNodeAck(const std::string& node,
                                                   Tracking tracking,
                                                   int64_t seqno) {
    const auto& pos_ = firstChain->positions.at(node);
    auto& pos = const_cast<Position&>(tracking == Tracking::Memory ? pos_.memory
                                                                   : pos_.disk);

    // Note: using WeaklyMonotonic, as receiving the same seqno multiple times
    // for the same node is ok. That just means that the node has not advanced
    // any of memory/disk seqnos.
    // E.g., imagine the following DCP_SEQNO_ACK sequence:
    //
    // {mem:1, disk:0} -> {mem:2, disk:0}
    //
    // That is legal, and it means that the node has enqueued seqnos {1, 2}
    // but not persisted anything yet.
    //
    // @todo: By doing this I don't catch the case where the replica has ack'ed
    //     both the same mem/disk seqnos twice (which shouldn't happen).
    //     It would be good to catch that, useful for replica logic-check.
    pos.lastAckSeqno = seqno;
}

ActiveDurabilityMonitor::NodeSeqnos ActiveDurabilityMonitor::getNodeWriteSeqnos(
        const std::string& node) const {
    return state.rlock()->getNodeWriteSeqnos(node);
}

ActiveDurabilityMonitor::NodeSeqnos ActiveDurabilityMonitor::getNodeAckSeqnos(
        const std::string& node) const {
    return state.rlock()->getNodeAckSeqnos(node);
}

const std::string& ActiveDurabilityMonitor::State::getActive() const {
    return firstChain->active;
}

ActiveDurabilityMonitor::NodeSeqnos
ActiveDurabilityMonitor::State::getNodeWriteSeqnos(
        const std::string& node) const {
    const auto& pos = firstChain->positions.at(node);
    return {pos.memory.lastWriteSeqno, pos.disk.lastWriteSeqno};
}

ActiveDurabilityMonitor::NodeSeqnos
ActiveDurabilityMonitor::State::getNodeAckSeqnos(
        const std::string& node) const {
    const auto& pos = firstChain->positions.at(node);
    return {pos.memory.lastAckSeqno, pos.disk.lastAckSeqno};
}

ActiveDurabilityMonitor::Container
ActiveDurabilityMonitor::State::removeSyncWrite(Container::iterator it) {
    if (it == trackedWrites.end()) {
        throw std::logic_error(
                "ActiveDurabilityMonitor::commit: Position points to end");
    }

    Container::iterator prev;
    // Note: iterators in trackedWrites are never singular, Container::end
    //     is used as placeholder element for when an iterator cannot point to
    //     any valid element in Container
    if (it == trackedWrites.begin()) {
        prev = trackedWrites.end();
    } else {
        prev = std::prev(it);
    }

    // Removing the element at 'it' from trackedWrites invalidates any
    // iterator that points to that element. So, we have to reposition the
    // invalidated iterators before proceeding with the removal.
    //
    // Note: O(N) with N=<number of iterators>, max(N)=12
    //     (max 2 chains, 3 replicas, 2 iterators per replica)
    for (const auto& entry : firstChain->positions) {
        const auto& nodePos = entry.second;
        if (nodePos.memory.it == it) {
            const_cast<Position&>(nodePos.memory).it = prev;
        }
        if (nodePos.disk.it == it) {
            const_cast<Position&>(nodePos.disk).it = prev;
        }
    }

    Container removed;
    removed.splice(removed.end(), trackedWrites, it);

    return removed;
}

void ActiveDurabilityMonitor::commit(const SyncWrite& sw) {
    const auto& key = sw.getKey();
    auto result = vb.commit(key,
                            sw.getBySeqno() /*prepareSeqno*/,
                            {} /*commitSeqno*/,
                            vb.lockCollections(key),
                            sw.getCookie());
    if (result != ENGINE_SUCCESS) {
        throw std::logic_error(
                "ActiveDurabilityMonitor::commit: VBucket::commit failed with "
                "status:" +
                std::to_string(result));
    }
}

void ActiveDurabilityMonitor::abort(const SyncWrite& sw) {
    const auto& key = sw.getKey();
    auto result = vb.abort(key,
                           sw.getBySeqno() /*prepareSeqno*/,
                           {} /*abortSeqno*/,
                           vb.lockCollections(key),
                           sw.getCookie());
    if (result != ENGINE_SUCCESS) {
        throw std::logic_error(
                "ActiveDurabilityMonitor::abort: VBucket::abort failed with "
                "status:" +
                std::to_string(result));
    }
}

void ActiveDurabilityMonitor::State::processSeqnoAck(const std::string& node,
                                                     Tracking tracking,
                                                     int64_t ackSeqno,
                                                     Container& toCommit) {
    if (!firstChain) {
        throw std::logic_error(
                "ActiveDurabilityMonitor::processSeqnoAck: FirstChain not "
                "set");
    }

    // Note: process up to the ack'ed seqno
    ActiveDurabilityMonitor::Container::iterator next;
    while ((next = getNodeNext(node, tracking)) != trackedWrites.end() &&
           next->getBySeqno() <= ackSeqno) {
        // Update replica tracking
        advanceNodePosition(node, tracking);

        const auto& nodePos = firstChain->positions.at(node);
        const auto& pos =
                (tracking == Tracking::Memory ? nodePos.memory : nodePos.disk);

        // Check if Durability Requirements satisfied now, and add for commit
        if (pos.it->isSatisfied()) {
            auto removed = removeSyncWrite(pos.it);
            toCommit.splice(toCommit.end(), removed);
        }
    }

    // We keep track of the actual ack'ed seqno
    updateNodeAck(node, tracking, ackSeqno);
}

std::unordered_set<int64_t> ActiveDurabilityMonitor::getTrackedSeqnos() const {
    const auto s = state.rlock();
    std::unordered_set<int64_t> ret;
    for (const auto& w : s->trackedWrites) {
        ret.insert(w.getBySeqno());
    }
    return ret;
}

size_t ActiveDurabilityMonitor::wipeTracked() {
    auto s = state.wlock();
    // Note: Cannot just do Container::clear as it would invalidate every
    //     existing Replication Chain iterator
    size_t removed{0};
    Container::iterator it = s->trackedWrites.begin();
    while (it != s->trackedWrites.end()) {
        // Note: 'it' will be invalidated, so it will need to be reset
        const auto next = std::next(it);
        removed += s->removeSyncWrite(it).size();
        it = next;
    }
    return removed;
}

void ActiveDurabilityMonitor::toOStream(std::ostream& os) const {
    const auto s = state.rlock();
    os << "ActiveDurabilityMonitor[" << this
       << "] #trackedWrites:" << s->trackedWrites.size() << "\n";
    for (const auto& w : s->trackedWrites) {
        os << "    " << w << "\n";
    }
    os << "]";
}

void ActiveDurabilityMonitor::State::setReplicationTopology(
        const nlohmann::json& topology) {
    // @todo: Add support for SecondChain
    std::vector<std::string> fChain;
    for (auto& node : topology.at(0).items()) {
        // First node (active) must be present, remaining (replica) nodes
        // are allowed to be Null indicating they are undefined.
        if (node.value().is_string()) {
            fChain.push_back(node.value());
        } else {
            fChain.emplace_back(UndefinedNode);
        }
    }
    // Note: Topology changes (i.e., reset of replication-chain) are implicitly
    //     supported. With the current model the new replication-chain will
    //     kick-in at the first new SyncWrite added to tracking.
    // @todo: Check if the above is legal
    firstChain =
            std::make_unique<ReplicationChain>(fChain, trackedWrites.begin());
}

void ActiveDurabilityMonitor::State::addSyncWrite(const void* cookie,
                                                  const queued_item& item) {
    trackedWrites.emplace_back(cookie, item, *firstChain);
    lastTrackedSeqno = item->getBySeqno();
}

void ActiveDurabilityMonitor::State::removeExpired(
        std::chrono::steady_clock::time_point asOf, Container& expired) {
    Container::iterator it = trackedWrites.begin();
    while (it != trackedWrites.end()) {
        if (it->isExpired(asOf)) {
            // Note: 'it' will be invalidated, so it will need to be reset
            const auto next = std::next(it);

            auto removed = removeSyncWrite(it);
            expired.splice(expired.end(), removed);

            it = next;
        } else {
            ++it;
        }
    }
}