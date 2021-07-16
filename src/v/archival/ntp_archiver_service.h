/*
 * Copyright 2021 Vectorized, Inc.
 *
 * Licensed as a Redpanda Enterprise file under the Redpanda Community
 * License (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 *
 * https://github.com/vectorizedio/redpanda/blob/master/licenses/rcl.md
 */

#pragma once
#include "archival/archival_policy.h"
#include "archival/probe.h"
#include "archival/types.h"
#include "cloud_storage/manifest.h"
#include "cloud_storage/remote.h"
#include "cloud_storage/types.h"
#include "model/fundamental.h"
#include "model/metadata.h"
#include "s3/client.h"
#include "storage/fwd.h"
#include "storage/segment.h"
#include "utils/retry_chain_node.h"

#include <seastar/core/abort_source.hh>
#include <seastar/core/lowres_clock.hh>
#include <seastar/core/semaphore.hh>
#include <seastar/core/shared_ptr.hh>

#include <functional>
#include <map>

namespace archival {

using namespace std::chrono_literals;

/// Archiver service configuration
struct configuration {
    /// S3 configuration
    s3::configuration client_config;
    /// Bucket used to store all archived data
    s3::bucket_name bucket_name;
    /// Time interval to run uploads & deletes
    ss::lowres_clock::duration interval;
    /// Number of simultaneous S3 uploads
    s3_connection_limit connection_limit;
    /// Initial backoff for uploads
    ss::lowres_clock::duration initial_backoff;
    /// Long upload timeout
    ss::lowres_clock::duration segment_upload_timeout;
    /// Shor upload timeout
    ss::lowres_clock::duration manifest_upload_timeout;
    /// Flag that indicates that service level metrics are disabled
    service_metrics_disabled svc_metrics_disabled;
    /// Flag that indicates that ntp-archiver level metrics are disabled
    per_ntp_metrics_disabled ntp_metrics_disabled;
};

std::ostream& operator<<(std::ostream& o, const configuration& cfg);

/// This class performs per-ntp arhcival workload. Every ntp can be
/// processed independently, without the knowledge about others. All
/// 'ntp_archiver' instances that the shard posesses are supposed to be
/// aggregated on a higher level in the 'archiver_service'.
///
/// The 'ntp_archiver' is responsible for manifest manitpulations and
/// generation of per-ntp candidate set. The actual file uploads are
/// handled by 'archiver_service'.
class ntp_archiver {
public:
    /// Iterator type used to retrieve candidates for upload
    using back_insert_iterator
      = std::back_insert_iterator<std::vector<segment_name>>;

    /// Create new instance
    ///
    /// \param ntp is an ntp that archiver is responsible for
    /// \param conf is an S3 client configuration
    /// \param remote is an object used to send/recv data
    /// \param svc_probe is a service level probe (optional)
    ntp_archiver(
      const storage::ntp_config& ntp,
      const configuration& conf,
      cloud_storage::remote& remote,
      service_probe& svc_probe);

    /// Stop archiver.
    ///
    /// \return future that will become ready when all async operation will be
    /// completed
    ss::future<> stop();

    /// Get NTP
    const model::ntp& get_ntp() const;

    /// Get revision id
    model::revision_id get_revision_id() const;

    /// Get timestamp
    const ss::lowres_clock::time_point get_last_upload_time() const;

    /// Download manifest from pre-defined S3 locatnewion
    ///
    /// \return future that returns true if the manifest was found in S3
    ss::future<cloud_storage::download_result>
    download_manifest(retry_chain_node& parent);

    /// Upload manifest to the pre-defined S3 location
    ss::future<cloud_storage::upload_result>
    upload_manifest(retry_chain_node& parent);

    const cloud_storage::manifest& get_remote_manifest() const;

    struct batch_result {
        size_t num_succeded;
        size_t num_failed;
    };

    /// \brief Upload next set of segments to S3 (if any)
    /// The semaphore is used to track number of parallel uploads. The method
    /// will pick not more than '_concurrency' candidates and start
    /// uploading them.
    ///
    /// \param lm is a log manager instance
    /// \param high_watermark is a high watermark offset of the partition
    /// \param parent is a retry chain node of the caller
    /// \return future that returns number of uploaded/failed segments
    ss::future<batch_result> upload_next_candidates(
      storage::log_manager& lm,
      model::offset high_watermark,
      retry_chain_node& parent);

private:
    /// Upload individual segment to S3.
    ///
    /// \return true on success and false otherwise
    ss::future<cloud_storage::upload_result>
    upload_segment(upload_candidate candidate, retry_chain_node& fib);

    service_probe& _svc_probe;
    ntp_level_probe _probe;
    model::ntp _ntp;
    model::revision_id _rev;
    cloud_storage::remote& _remote;
    archival_policy _policy;
    s3::bucket_name _bucket;
    /// Remote manifest contains representation of the data stored in S3 (it
    /// gets uploaded to the remote location)
    cloud_storage::manifest _manifest;
    ss::gate _gate;
    ss::abort_source _as;
    ss::semaphore _mutex{1};
    simple_time_jitter<ss::lowres_clock> _backoff{100ms};
    size_t _concurrency{4};
    ss::lowres_clock::time_point _last_upload_time;
    ss::lowres_clock::duration _initial_backoff;
    ss::lowres_clock::duration _segment_upload_timeout;
    ss::lowres_clock::duration _manifest_upload_timeout;
};

} // namespace archival
