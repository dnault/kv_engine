/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2020 Couchbase, Inc.
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

/**
 * Static definitions for statistics.
 *
 * Requires a STAT macro be defined before including this file.
 * STAT(uniqueName, ...)
 *
 * where:
 *  * uniqueName - a key which identifies the stat (used as the enum value
 *                  and cbstats key)
 * and the remaining optional parameters are:
 *  * unit - name of a cb::stats::Unit which identifies what unit the stat
 *           value represents (e.g., microseconds). default: none
 *  * familyName - the metric name used by Prometheus. This need _not_ be
 * unique, and can be shared by stats which are distinguishable by labels.
 * default: uniqueName
 *  * labelKey - key of a label to be applied to the stat. default:""
 *  * labelValue - value of a label to be applied to the stat. default:""
 *
 * Stats should be formatted as
 *
 * STAT(unique_name, unit, family_name, labelKey, labelValue)
 *
 * e.g.,
 *
 * STAT(get_cmd, microseconds, cmd_time_taken, op, get)
 * STAT(set_cmd, microseconds, cmd_time_taken, op, set)
 *
 * The uniqueName will be used as an enum key, and as the stat key for
 * backends which do not support labelled stat families (e.g., CBStats).
 *
 * The familyName and labels will be used by backends which _do_ support
 * them, like Prometheus. All stats of a given family_name should be of the same
 * unit (e.g., count, bytes, seconds, kilobytes per microsecond) and it should
 * be possible to meaningfully aggregate the stat values e.g., get_cmd and
 * set_cmd can be summed.
 *
 * Only uniqueName is mandatory. The minimal definition of a stat is therefore
 * STAT(uniqueName)
 * For stats with unspecified units and no labels. In this case, the uniqueName
 * will also be used as the familyName.
 *
 */

#ifndef STAT
#warning A STAT macro must be defined before including stats.def.h
// If this header is being checked by clang-tidy or similar _not_ as part of
// definitions.h, there will not be a STAT definition.
// Define an empty STAT macro to avoid unnecessary warnings.
#define STAT(...)
#endif

// include generated config STAT declarations
#include "stats_config.def.h" // NOLINT(*)

// TODO: applying a "kv_" prefix globally would be consistent but lead to kv_ep_
//  for some stats. Providing metric family names without ep_ would avoid this
// "All" stats group (doEngineStats)
STAT(ep_storage_age, microseconds, , , )
STAT(ep_storage_age_highwat, microseconds, , , )
STAT(ep_num_workers, count, , , )
STAT(ep_bucket_priority,
     none,
     ,
     , ) // TODO: make 0/1 rather than text for Prometheus?
STAT(ep_total_enqueued, count, , , )
STAT(ep_total_deduplicated, count, , , )
STAT(ep_expired_access, count, , , )
STAT(ep_expired_compactor, count, , , )
STAT(ep_expired_pager, count, , , )
STAT(ep_queue_size, count, , , )
STAT(ep_diskqueue_items, count, , , )
STAT(ep_commit_num, count, , , )
STAT(ep_commit_time, microseconds, , , )
STAT(ep_commit_time_total, microseconds, , , )
STAT(ep_item_begin_failed, count, , , )
STAT(ep_item_commit_failed, count, , , )
STAT(ep_item_flush_expired, count, , , )
STAT(ep_item_flush_failed, count, , , )
STAT(ep_flusher_state, none, , , )
STAT(ep_flusher_todo, count, , , )
STAT(ep_total_persisted, count, , , )
STAT(ep_uncommitted_items, count, , , )
STAT(ep_chk_persistence_timeout, seconds, , , )
STAT(ep_vbucket_del, count, , , )
STAT(ep_vbucket_del_fail, count, , , )
STAT(ep_flush_duration_total, milliseconds, , , )
STAT(ep_persist_vbstate_total, count, , , )
STAT(mem_used, bytes, , , )
STAT(mem_used_estimate, bytes, , , )
STAT(ep_mem_low_wat_percent, percent, , , )
STAT(ep_mem_high_wat_percent, percent, , , )
/* TODO: it's not advised to have metric like:
 *   my_metric{label=a} 1
 *   my_metric{label=b} 6
 *   my_metric{label=total} 7
 *  as a total is inconvenient for aggregation, _but_ we do track
 * several stats which are logically totals which might include things _not_
 * available under any other metric. Exposing it under a different metric name
 * seems best. Note: "..._total" is expected to be reserved for Counters -
 * totals over time, not totals of other things.
 */
STAT(bytes, bytes, total_memory_used, , )
STAT(ep_kv_size, bytes, memory_used, for, hashtable)
STAT(ep_blob_num, count, , , )
STAT(ep_blob_overhead,
     bytes,
     memory_overhead,
     for, blobs) // TODO: Assess what labels would actually be _useful_ for querying
STAT(ep_value_size,
     bytes,
     memory_used,
     for, blobs)
STAT(ep_storedval_size, bytes, memory_used, for, storedvalues)
STAT(ep_storedval_overhead, bytes, memory_overhead, for, storedvalues)
STAT(ep_storedval_num, count, , , )
STAT(ep_overhead, bytes, total_memory_overhead, , )
STAT(ep_item_num, count, , , )
STAT(ep_oom_errors, count, , , )
STAT(ep_tmp_oom_errors, count, , , )
STAT(ep_mem_tracker_enabled, none, , , )
STAT(ep_bg_fetched, count, , , )
STAT(ep_bg_meta_fetched, count, , , )
STAT(ep_bg_remaining_items, count, , , )
STAT(ep_bg_remaining_jobs, count, , , )
STAT(ep_num_pager_runs, count, , , )
STAT(ep_num_expiry_pager_runs, count, , , )
STAT(ep_num_freq_decayer_runs, count, , , )
STAT(ep_items_expelled_from_checkpoints, count, , , )
STAT(ep_items_rm_from_checkpoints, count, , , )
STAT(ep_num_value_ejects, count, , , )
STAT(ep_num_eject_failures, count, , , )
STAT(ep_num_not_my_vbuckets, count, , , )
STAT(ep_pending_ops, count, , , )
STAT(ep_pending_ops_total,
     count,
     ,
     , ) // TODO: are total-over-uptime stats relevant for prometheus
         //  given the ability to sum over a time period?
STAT(ep_pending_ops_max,
     count,
     ,
     , ) // TODO: standardise labelling for "high watermark" style stats
STAT(ep_pending_ops_max_duration, microseconds, , , )
STAT(ep_pending_compactions, count, , , )
STAT(ep_rollback_count, count, , , )
STAT(ep_vbucket_del_max_walltime, microseconds, , , )
STAT(ep_vbucket_del_avg_walltime, microseconds, , , )
STAT(ep_bg_num_samples, count, , , )
STAT(ep_bg_min_wait, microseconds, , , )
STAT(ep_bg_max_wait, microseconds, , , )
STAT(ep_bg_wait_avg, microseconds, , , ) // TODO: derived from two stats. Decide
                                         //  whether to expose for prometheus
STAT(ep_bg_min_load, microseconds, , , )
STAT(ep_bg_max_load, microseconds, , , )
STAT(ep_bg_load_avg, microseconds, , , ) // TODO: derived from two stats. Decide
                                         //  whether to expose for prometheus
STAT(ep_bg_wait, microseconds, , , )
STAT(ep_bg_load, microseconds, , , )
STAT(ep_degraded_mode, none, , , )
STAT(ep_num_access_scanner_runs, count, , , )
STAT(ep_num_access_scanner_skips, count, , , )
STAT(ep_access_scanner_last_runtime,
     seconds,
     ,
     , ) // TODO: relative to server start. Convert to absolute time?
STAT(ep_access_scanner_num_items, count, , , )
STAT(ep_access_scanner_task_time,
     none,
     ,
     , ) // TODO: this is a string, expose numeric time for Prometheus
STAT(ep_expiry_pager_task_time,
     none,
     ,
     , ) // TODO: this is a string, expose numeric time for Prometheus
STAT(ep_startup_time, seconds, , , )
STAT(ep_warmup_thread, none, , , )
STAT(ep_warmup_time, microseconds, , , )
STAT(ep_warmup_oom, count, , , )
STAT(ep_warmup_dups, count, , , )
STAT(ep_num_ops_get_meta, count, num_ops, op, get_meta)
STAT(ep_num_ops_set_meta, count, num_ops, op, set_meta)
STAT(ep_num_ops_del_meta, count, num_ops, op, del_meta)
STAT(ep_num_ops_set_meta_res_fail, count, num_ops_failed, op, set_meta)
STAT(ep_num_ops_del_meta_res_fail, count, num_ops_failed, op, del_meta)
STAT(ep_num_ops_set_ret_meta, count, num_ops, op, set_ret_meta)
STAT(ep_num_ops_del_ret_meta, count, num_ops, op, del_ret_meta)
STAT(ep_num_ops_get_meta_on_set_meta, count, num_ops, op, get_meta)
STAT(ep_workload_pattern, none, , , )
STAT(ep_defragmenter_num_visited, count, , , )
STAT(ep_defragmenter_num_moved, count, , , )
STAT(ep_defragmenter_sv_num_moved, count, , , )
STAT(ep_item_compressor_num_visited, count, , , )
STAT(ep_item_compressor_num_compressed, count, , , )
STAT(ep_cursor_dropping_lower_threshold, bytes, , , )
STAT(ep_cursor_dropping_upper_threshold, bytes, , , )
STAT(ep_cursors_dropped, count, , , )
STAT(ep_cursor_memory_freed, bytes, , , )
STAT(ep_data_write_failed, count, , , )
STAT(ep_data_read_failed, count, , , )
STAT(ep_io_document_write_bytes, bytes, , , )
STAT(ep_io_total_read_bytes, bytes, , , )
STAT(ep_io_total_write_bytes, bytes, , , )
STAT(ep_io_compaction_read_bytes, bytes, , , )
STAT(ep_io_compaction_write_bytes, bytes, , , )
STAT(ep_io_bg_fetch_read_count, count, , , )
STAT(ep_bg_fetch_avg_read_amplification, ratio, , , )
STAT(ep_rocksdb_kMemTableTotal, bytes, , , )
STAT(ep_rocksdb_kMemTableUnFlushed, bytes, , , )
STAT(ep_rocksdb_kTableReadersTotal, bytes, , , )
STAT(ep_rocksdb_kCacheTotal, bytes, , , )
STAT(ep_rocksdb_default_kSizeAllMemTables, bytes, , , )
STAT(ep_rocksdb_seqno_kSizeAllMemTables, bytes, , , )
STAT(ep_rocksdb_block_cache_data_hit_ratio, ratio, , , )
STAT(ep_rocksdb_block_cache_index_hit_ratio, ratio, , , )
STAT(ep_rocksdb_block_cache_filter_hit_ratio, ratio, , , )
STAT(ep_rocksdb_default_kTotalSstFilesSize, bytes, , , )
STAT(ep_rocksdb_seqno_kTotalSstFilesSize, bytes, , , )
STAT(ep_rocksdb_scan_totalSeqnoHits, count, , , )
STAT(ep_rocksdb_scan_oldSeqnoHits, count, , , )

// EPBucket::getFileStats
STAT(ep_db_data_size, bytes, , , )
STAT(ep_db_file_size, bytes, , , )

// Timing stats
STAT(bg_wait, microseconds, , , )
STAT(bg_load, microseconds, , , )
STAT(set_with_meta, microseconds, , , )
STAT(pending_ops, microseconds, , , )
STAT(access_scanner, microseconds, , , )
STAT(checkpoint_remover, microseconds, , , )
STAT(item_pager, microseconds, , , )
STAT(expiry_pager, microseconds, , , )
STAT(storage_age, microseconds, , , )
STAT(get_cmd, microseconds, cmd_time_taken, op, get)
STAT(store_cmd, microseconds, cmd_time_taken, op, store)
STAT(arith_cmd, microseconds, cmd_time_taken, op, arith)
STAT(get_stats_cmd, microseconds, cmd_time_taken, op, get_stats)
STAT(get_vb_cmd, microseconds, cmd_time_taken, op, get_vb)
STAT(set_vb_cmd, microseconds, cmd_time_taken, op, set_vb)
STAT(del_vb_cmd, microseconds, cmd_time_taken, op, del_vb)
STAT(chk_persistence_cmd, microseconds, cmd_time_taken, op, chk_persistence)
STAT(notify_io, microseconds, , , )
STAT(batch_read, microseconds, , , )
STAT(disk_insert, microseconds, disk, op, insert)
STAT(disk_update, microseconds, disk, op, update)
STAT(disk_del, microseconds, disk, op, del)
STAT(disk_vb_del, microseconds, disk, op, vb_del)
STAT(disk_commit, microseconds, disk, op, commit)
STAT(item_alloc_sizes,
     bytes,
     ,
     , ) // TODO: this is not timing related but is in doTimingStats
STAT(bg_batch_size,
     count,
     ,
     , ) // TODO: this is not timing related but is in doTimingStats
STAT(persistence_cursor_get_all_items,
     microseconds,
     cursor_get_all_items_time,
     cursor_type,
     persistence)
STAT(dcp_cursors_get_all_items,
     microseconds,
     cursor_get_all_items_time,
     cursor_type,
     dcp)
STAT(sync_write_commit_majority,
     microseconds,
     sync_write_commit_duration,
     level,
     majority)
STAT(sync_write_commit_majority_and_persist_on_master,
     microseconds,
     sync_write_commit_duration,
     level,
     majority_and_persist_on_master)
STAT(sync_write_commit_persist_to_majority,
     microseconds,
     sync_write_commit_duration,
     level,
     persist_to_majority)

// server_stats
STAT(uptime, seconds, , , )
STAT(stat_reset,
     none,
     ,
     , ) // TODO: String indicating when stats were reset. Change
         //  to a numeric stat for Prometheus?
STAT(time, seconds, , , )
STAT(version, none, , , ) // version string
STAT(memcached_version, none, , , ) // version string
STAT(daemon_connections, count, , , )
STAT(curr_connections, count, , , )
STAT(system_connections, count, , , )
STAT(total_connections, count, , , ) // total since start/reset
STAT(connection_structures, count, , , )
STAT(cmd_get, count, operations, op, get)
STAT(cmd_set, count, operations, op, set)
STAT(cmd_flush, count, operations, op, flush)
STAT(cmd_lock, count, operations, op, lock)
STAT(cmd_subdoc_lookup, count, subdoc_operations, op, lookup)
STAT(cmd_subdoc_mutation, count, subdoc_operations, op, mutation)
STAT(bytes_subdoc_lookup_total,
     bytes,
     subdoc_lookup_searched,
     , ) // type _bytes will be suffixed
STAT(bytes_subdoc_lookup_extracted, bytes, subdoc_lookup_extracted, , )
STAT(bytes_subdoc_mutation_total, bytes, subdoc_mutation_updated, , )
STAT(bytes_subdoc_mutation_inserted, bytes, subdoc_mutation_inserted, , )
// aggregates over all buckets
STAT(cmd_total_sets, count, , , )
STAT(cmd_total_gets, count, , , )
STAT(cmd_total_ops, count, , , )
// aggregates over multiple operations for a single bucket
STAT(cmd_mutation, count, , , )
STAT(cmd_lookup, count, , , )

STAT(auth_cmds, count, , , )
STAT(auth_errors, count, , , )
STAT(get_hits, count, , , )
STAT(get_misses, count, , , )
STAT(delete_misses, count, , , )
STAT(delete_hits, count, , , )
STAT(incr_misses, count, , , )
STAT(incr_hits, count, , , )
STAT(decr_misses, count, , , )
STAT(decr_hits, count, , , )
STAT(cas_misses, count, , , )
STAT(cas_hits, count, , , )
STAT(cas_badval, count, , , )
STAT(bytes_read, bytes, read, , ) // type _bytes will be suffixed
STAT(bytes_written, bytes, written, , )
STAT(rejected_conns, count, , , )
STAT(threads, count, , , )
STAT(conn_yields, count, , , )
STAT(iovused_high_watermark, none, , , )
STAT(msgused_high_watermark, none, , , )
STAT(lock_errors, count, , , )
STAT(cmd_lookup_10s_count, count, , , )
// us suffix would be confusing in Prometheus as the stat is scaled to seconds
STAT(cmd_lookup_10s_duration_us, microseconds, cmd_lookup_10s_duration, , )
STAT(cmd_mutation_10s_count, count, , , )
// us suffix would be confusing in Prometheus as the stat is scaled to seconds
STAT(cmd_mutation_10s_duration_us, microseconds, cmd_mutation_10s_duration, , )
STAT(total_resp_errors, count, , , )

// Vbucket aggreagated stats
#define VB_AGG_STAT(name, unit, familyName)                   \
    STAT(vb_active_##name, unit, familyName, state, active)   \
    STAT(vb_replica_##name, unit, familyName, state, replica) \
    STAT(vb_pending_##name, unit, name, state, pending)

VB_AGG_STAT(num, count, num_vbuckets)
VB_AGG_STAT(curr_items, count, )
VB_AGG_STAT(hp_vb_req_size, count, num_high_pri_requests)
VB_AGG_STAT(num_non_resident, count, )
VB_AGG_STAT(perc_mem_resident, percent, )
VB_AGG_STAT(eject, count, )
VB_AGG_STAT(expired, count, )
VB_AGG_STAT(meta_data_memory, bytes, )
VB_AGG_STAT(meta_data_disk, bytes, )
VB_AGG_STAT(checkpoint_memory, bytes, )
VB_AGG_STAT(checkpoint_memory_unreferenced, bytes, )
VB_AGG_STAT(checkpoint_memory_overhead, bytes, )
VB_AGG_STAT(ht_memory, bytes, )
VB_AGG_STAT(itm_memory, bytes, )
VB_AGG_STAT(itm_memory_uncompressed, bytes, )
VB_AGG_STAT(ops_create, count, )
VB_AGG_STAT(ops_update, count, )
VB_AGG_STAT(ops_delete, count, )
VB_AGG_STAT(ops_get, count, )
VB_AGG_STAT(ops_reject, count, )
VB_AGG_STAT(queue_size, count, )
VB_AGG_STAT(queue_memory, bytes, )
VB_AGG_STAT(queue_age, milliseconds, )
VB_AGG_STAT(queue_pending, bytes, )
VB_AGG_STAT(queue_fill, count, )
VB_AGG_STAT(queue_drain, count, )
VB_AGG_STAT(rollback_item_count, count, )

#undef VB_AGG_STAT

STAT(curr_items, count, , , )
STAT(curr_temp_items, count, , , )
STAT(curr_items_tot, count, , , )

STAT(vb_active_sync_write_accepted_count, count, , , )
STAT(vb_active_sync_write_committed_count, count, , , )
STAT(vb_active_sync_write_aborted_count, count, , , )
STAT(vb_replica_sync_write_accepted_count, count, , , )
STAT(vb_replica_sync_write_committed_count, count, , , )
STAT(vb_replica_sync_write_aborted_count, count, , , )
STAT(vb_dead_num, count, , , )
STAT(ep_vb_total, count, , , )
STAT(ep_total_new_items, count, , , )
STAT(ep_total_del_items, count, , , )
STAT(ep_diskqueue_memory, bytes, , , )
STAT(ep_diskqueue_fill, count, , , )
STAT(ep_diskqueue_drain, count, , , )
STAT(ep_diskqueue_pending, count, , , )
STAT(ep_meta_data_memory, bytes, , , )
STAT(ep_meta_data_disk, bytes, , , )
STAT(ep_checkpoint_memory, bytes, , , )
STAT(ep_checkpoint_memory_unreferenced, bytes, , , )
STAT(ep_checkpoint_memory_overhead, bytes, , , )
STAT(ep_total_cache_size, bytes, , , )
STAT(rollback_item_count, count, , , )
STAT(ep_num_non_resident, count, , , )
STAT(ep_chk_persistence_remains, count, , , )
STAT(ep_active_hlc_drift, microseconds, , , )
STAT(ep_active_hlc_drift_count, count, , , )
STAT(ep_replica_hlc_drift, microseconds, , , )
STAT(ep_replica_hlc_drift_count, count, , , )
STAT(ep_active_ahead_exceptions, count, , , )
STAT(ep_active_behind_exceptions, count, , , )
STAT(ep_replica_ahead_exceptions, count, , , )
STAT(ep_replica_behind_exceptions, count, , , )
STAT(ep_clock_cas_drift_threshold_exceeded, count, , , )
