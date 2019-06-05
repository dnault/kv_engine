/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2012 Couchbase, Inc
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

#include "couch-kvstore/couch-fs-stats.h"
#include "common.h"
#include "kvstore.h"
#include <platform/histogram.h>

std::unique_ptr<FileOpsInterface> getCouchstoreStatsOps(
    FileStats& stats, FileOpsInterface& base_ops) {
    return std::unique_ptr<FileOpsInterface>(new StatsOps(stats, base_ops));
}

StatsOps::StatFile::StatFile(FileOpsInterface* _orig_ops,
                             couch_file_handle _orig_handle,
                             cs_off_t _last_offs)
    : orig_ops(_orig_ops),
      orig_handle(_orig_handle),
      last_offs(_last_offs),
      read_count_since_open(0),
      write_count_since_open(0) {
}

size_t StatsOps::StatFile::getReadCount() {
    return read_count_since_open;
}

size_t StatsOps::StatFile::getWriteCount() {
    return write_count_since_open;
}

couch_file_handle StatsOps::constructor(couchstore_error_info_t *errinfo) {
    FileOpsInterface* orig_ops = &wrapped_ops;
    StatFile* sf = new StatFile(orig_ops,
                                orig_ops->constructor(errinfo),
                                0);
    return reinterpret_cast<couch_file_handle>(sf);
}

couchstore_error_t StatsOps::open(couchstore_error_info_t* errinfo,
                                  couch_file_handle* h,
                                  const char* path,
                                  int flags) {
    StatFile* sf = reinterpret_cast<StatFile*>(*h);
    sf->read_count_since_open = 0;
    sf->write_count_since_open = 0;
    return sf->orig_ops->open(errinfo, &sf->orig_handle, path, flags);
}

couchstore_error_t StatsOps::close(couchstore_error_info_t* errinfo,
                                   couch_file_handle h) {
    StatFile* sf = reinterpret_cast<StatFile*>(h);
    // Add to histograms - we can have zero read (open, goto_eof and close for
    // size; or on error); or zero write (read-only activity) - so only added if
    // counts are non-zero.
    if (sf->read_count_since_open > 0) {
        stats.readCountHisto.add(sf->read_count_since_open);
    }
    if (sf->write_count_since_open > 0) {
        stats.writeCountHisto.add(sf->write_count_since_open);
    }

    return sf->orig_ops->close(errinfo, sf->orig_handle);
}

couchstore_error_t StatsOps::set_periodic_sync(couch_file_handle h,
                                               uint64_t period_bytes) {
    StatFile* sf = reinterpret_cast<StatFile*>(h);
    return sf->orig_ops->set_periodic_sync(sf->orig_handle, period_bytes);
}

ssize_t StatsOps::pread(couchstore_error_info_t* errinfo,
                        couch_file_handle h,
                        void* buf,
                        size_t sz,
                        cs_off_t off) {
    StatFile* sf = reinterpret_cast<StatFile*>(h);
    stats.readSizeHisto.add(sz);
    if(sf->last_offs) {
        stats.readSeekHisto.add(std::abs(off - sf->last_offs));
    }
    sf->last_offs = off;
    HdrMicroSecBlockTimer bt(&stats.readTimeHisto);
    ssize_t result = sf->orig_ops->pread(errinfo, sf->orig_handle, buf,
                                         sz, off);
    if (result > 0) {
        stats.totalBytesRead += result;
        ++sf->read_count_since_open;
    }
    return result;
}

ssize_t StatsOps::pwrite(couchstore_error_info_t*errinfo,
                         couch_file_handle h,
                         const void* buf,
                         size_t sz,
                         cs_off_t off) {
    StatFile* sf = reinterpret_cast<StatFile*>(h);
    stats.writeSizeHisto.add(sz);
    HdrMicroSecBlockTimer bt(&stats.writeTimeHisto);
    ssize_t result = sf->orig_ops->pwrite(errinfo, sf->orig_handle, buf,
                                          sz, off);
    if (result > 0) {
        stats.totalBytesWritten += result;
        ++sf->write_count_since_open;
    }
    return result;
}

cs_off_t StatsOps::goto_eof(couchstore_error_info_t* errinfo,
                            couch_file_handle h) {
    StatFile* sf = reinterpret_cast<StatFile*>(h);
    return sf->orig_ops->goto_eof(errinfo, sf->orig_handle);
}

couchstore_error_t StatsOps::sync(couchstore_error_info_t* errinfo,
                                  couch_file_handle h) {
    StatFile* sf = reinterpret_cast<StatFile*>(h);
    HdrMicroSecBlockTimer bt(&stats.syncTimeHisto);
    return sf->orig_ops->sync(errinfo, sf->orig_handle);
}

couchstore_error_t StatsOps::advise(couchstore_error_info_t* errinfo,
                                    couch_file_handle h,
                                    cs_off_t offs,
                                    cs_off_t len,
                                    couchstore_file_advice_t adv) {
    StatFile* sf = reinterpret_cast<StatFile*>(h);
    return sf->orig_ops->advise(errinfo, sf->orig_handle, offs, len, adv);
}

FileOpsInterface::FHStats* StatsOps::get_stats(couch_file_handle h) {
    // StatFile implements FHStats interface directly.
    StatFile* sf = reinterpret_cast<StatFile*>(h);
    return sf;
}

void StatsOps::destructor(couch_file_handle h) {
    StatFile* sf = reinterpret_cast<StatFile*>(h);
    sf->orig_ops->destructor(sf->orig_handle);
    delete sf;
}

