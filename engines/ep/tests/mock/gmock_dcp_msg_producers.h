/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2019 Couchbase, Inc
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

#include <folly/portability/GMock.h>
#include <memcached/dcp.h>
#include <memcached/durability_spec.h>
#include <memcached/engine.h>

class Item;

/**
 * Mock of dcp_messsage_producers, using GoogleMock to mock.
 */
class GMockDcpMsgProducers : public dcp_message_producers {
public:
    MOCK_METHOD(ENGINE_ERROR_CODE,
                get_failover_log,
                (uint32_t opaque, Vbid vbucket),
                (override));

    MOCK_METHOD(ENGINE_ERROR_CODE,
                stream_req,
                (uint32_t opaque,
                 Vbid vbucket,
                 uint32_t flags,
                 uint64_t start_seqno,
                 uint64_t end_seqno,
                 uint64_t vbucket_uuid,
                 uint64_t snap_start_seqno,
                 uint64_t snap_end_seqno,
                 const std::string& request_value),
                (override));

    MOCK_METHOD(ENGINE_ERROR_CODE,
                add_stream_rsp,
                (uint32_t opaque,
                 uint32_t stream_opaque,
                 cb::mcbp::Status status),
                (override));

    MOCK_METHOD(ENGINE_ERROR_CODE,
                marker_rsp,
                (uint32_t opaque, cb::mcbp::Status status),
                (override));

    MOCK_METHOD(ENGINE_ERROR_CODE,
                set_vbucket_state_rsp,
                (uint32_t opaque, cb::mcbp::Status status),
                (override));

    MOCK_METHOD(ENGINE_ERROR_CODE,
                stream_end,
                (uint32_t opaque,
                 Vbid vbucket,
                 cb::mcbp::DcpStreamEndStatus status,
                 cb::mcbp::DcpStreamId sid),
                (override));

    MOCK_METHOD(ENGINE_ERROR_CODE,
                marker,
                (uint32_t opaque,
                 Vbid vbucket,
                 uint64_t start_seqno,
                 uint64_t end_seqno,
                 uint32_t flags,
                 std::optional<uint64_t> high_completed_seqno,
                 std::optional<uint64_t> maxVisibleSeqno,
                 std::optional<uint64_t> timestamp,
                 cb::mcbp::DcpStreamId sid),
                (override));

    MOCK_METHOD(ENGINE_ERROR_CODE,
                mutation,
                (uint32_t opaque,
                 Item* itm,
                 Vbid vbucket,
                 uint64_t by_seqno,
                 uint64_t rev_seqno,
                 uint32_t lock_time,
                 uint8_t nru,
                 cb::mcbp::DcpStreamId sid));

    MOCK_METHOD(ENGINE_ERROR_CODE,
                deletion,
                (uint32_t opaque,
                 Item* itm,
                 Vbid vbucket,
                 uint64_t by_seqno,
                 uint64_t rev_seqno,
                 cb::mcbp::DcpStreamId sid));

    MOCK_METHOD(ENGINE_ERROR_CODE,
                deletionV2,
                (uint32_t opaque,
                 Item* itm,
                 Vbid vbucket,
                 uint64_t by_seqno,
                 uint64_t rev_seqno,
                 uint32_t delete_time,
                 cb::mcbp::DcpStreamId sid));

    MOCK_METHOD(ENGINE_ERROR_CODE,
                expiration,
                (uint32_t opaque,
                 Item* itm,
                 Vbid vbucket,
                 uint64_t by_seqno,
                 uint64_t rev_seqno,
                 uint32_t delete_time,
                 cb::mcbp::DcpStreamId sid));

    MOCK_METHOD(ENGINE_ERROR_CODE,
                set_vbucket_state,
                (uint32_t opaque, Vbid vbucket, vbucket_state_t state),
                (override));

    MOCK_METHOD(ENGINE_ERROR_CODE, noop, (uint32_t opaque), (override));

    MOCK_METHOD(ENGINE_ERROR_CODE,
                buffer_acknowledgement,
                (uint32_t opaque, Vbid vbucket, uint32_t buffer_bytes),
                (override));

    MOCK_METHOD(ENGINE_ERROR_CODE,
                control,
                (uint32_t opaque, std::string_view key, std::string_view value),
                (override));

    MOCK_METHOD(ENGINE_ERROR_CODE,
                system_event,
                (uint32_t opaque,
                 Vbid vbucket,
                 mcbp::systemevent::id event,
                 uint64_t bySeqno,
                 mcbp::systemevent::version version,
                 cb::const_byte_buffer key,
                 cb::const_byte_buffer eventData,
                 cb::mcbp::DcpStreamId sid),
                (override));

    MOCK_METHOD(ENGINE_ERROR_CODE,
                get_error_map,
                (uint32_t opaque, uint16_t version),
                (override));

    MOCK_METHOD(ENGINE_ERROR_CODE,
                prepare,
                (uint32_t opaque,
                 Item* itm,
                 Vbid vbucket,
                 uint64_t by_seqno,
                 uint64_t rev_seqno,
                 uint32_t lock_time,
                 uint8_t nru,
                 DocumentState document_state,
                 cb::durability::Level level));

    MOCK_METHOD(ENGINE_ERROR_CODE,
                seqno_acknowledged,
                (uint32_t opaque, Vbid vbucket, uint64_t prepared_seqno),
                (override));

    MOCK_METHOD(ENGINE_ERROR_CODE,
                commit,
                (uint32_t opaque,
                 Vbid vbucket,
                 const DocKey& key,
                 uint64_t prepare_seqno,
                 uint64_t commit_seqno),
                (override));

    MOCK_METHOD(ENGINE_ERROR_CODE,
                abort,
                (uint32_t opaque,
                 Vbid vbucket,
                 const DocKey& key,
                 uint64_t prepared_seqno,
                 uint64_t abort_seqno),
                (override));

    MOCK_METHOD(ENGINE_ERROR_CODE,
                oso_snapshot,
                (uint32_t opaque,
                 Vbid vbucket,
                 uint32_t flags,
                 cb::mcbp::DcpStreamId sid),
                (override));

    MOCK_METHOD(ENGINE_ERROR_CODE,
                seqno_advanced,
                (uint32_t opaque,
                 Vbid vbucket,
                 uint64_t prepared_seqno,
                 cb::mcbp::DcpStreamId sid),
                (override));

    // Current version of GMock doesn't support move-only types (e.g.
    // std::unique_ptr) for mocked function arguments. Workaround directly
    // implementing the affected methods (without GMock) and have them delegate
    // to a GMock method which takes a raw Item ptr.
    ENGINE_ERROR_CODE mutation(uint32_t opaque,
                               cb::unique_item_ptr itm,
                               Vbid vbucket,
                               uint64_t by_seqno,
                               uint64_t rev_seqno,
                               uint32_t lock_time,
                               uint8_t nru,
                               cb::mcbp::DcpStreamId sid) override {
        return mutation(opaque,
                        reinterpret_cast<Item*>(itm.get()),
                        vbucket,
                        by_seqno,
                        rev_seqno,
                        lock_time,
                        nru,
                        sid);
    }

    ENGINE_ERROR_CODE deletion(uint32_t opaque,
                               cb::unique_item_ptr itm,
                               Vbid vbucket,
                               uint64_t by_seqno,
                               uint64_t rev_seqno,
                               cb::mcbp::DcpStreamId sid) override {
        return deletion(opaque,
                        reinterpret_cast<Item*>(itm.get()),
                        vbucket,
                        by_seqno,
                        rev_seqno,
                        sid);
    }

    ENGINE_ERROR_CODE deletion_v2(uint32_t opaque,
                                  cb::unique_item_ptr itm,
                                  Vbid vbucket,
                                  uint64_t by_seqno,
                                  uint64_t rev_seqno,
                                  uint32_t delete_time,
                                  cb::mcbp::DcpStreamId sid) override {
        return deletionV2(opaque,
                          reinterpret_cast<Item*>(itm.get()),
                          vbucket,
                          by_seqno,
                          rev_seqno,
                          delete_time,
                          sid);
    }

    ENGINE_ERROR_CODE expiration(uint32_t opaque,
                                 cb::unique_item_ptr itm,
                                 Vbid vbucket,
                                 uint64_t by_seqno,
                                 uint64_t rev_seqno,
                                 uint32_t delete_time,
                                 cb::mcbp::DcpStreamId sid) override {
        return expiration(opaque,
                          reinterpret_cast<Item*>(itm.get()),
                          vbucket,
                          by_seqno,
                          rev_seqno,
                          delete_time,
                          sid);
    }

    ENGINE_ERROR_CODE prepare(uint32_t opaque,
                              cb::unique_item_ptr itm,
                              Vbid vbucket,
                              uint64_t by_seqno,
                              uint64_t rev_seqno,
                              uint32_t lock_time,
                              uint8_t nru,
                              DocumentState document_state,
                              cb::durability::Level level) override {
        return prepare(opaque,
                       reinterpret_cast<Item*>(itm.get()),
                       vbucket,
                       by_seqno,
                       rev_seqno,
                       lock_time,
                       nru,
                       document_state,
                       level);
    }
};
