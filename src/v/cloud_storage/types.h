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

#include "seastar/core/sstring.hh"
#include "seastarx.h"
#include "utils/named_type.h"

#include <filesystem>

namespace cloud_storage {

/// Segment file name without working directory,
/// expected format: <base-offset>-<term-id>-<revision>.log
using segment_name = named_type<ss::sstring, struct archival_segment_name_t>;
/// Segment path in S3, expected format:
/// <prefix>/<ns>/<topic>/<part-id>_<rev>/<base-offset>-<term-id>-<revision>.log
using remote_segment_path
  = named_type<std::filesystem::path, struct archival_remote_segment_path_t>;
using remote_manifest_path
  = named_type<std::filesystem::path, struct archival_remote_manifest_path_t>;
/// Local segment path, expected format:
/// <work-dir>/<ns>/<topic>/<part-id>_<rev>/<base-offset>-<term-id>-<revision>.log
using local_segment_path
  = named_type<std::filesystem::path, struct archival_local_segment_path_t>;
/// Number of simultaneous connections to S3
using s3_connection_limit
  = named_type<size_t, struct archival_s3_connection_limit_t>;

enum class download_result : int32_t {
    success,
    notfound,
    timedout,
    failed,
};

enum class upload_result : int32_t {
    success,
    timedout,
    failed,
};

enum class manifest_version : int32_t {
    v1 = 1,
};

enum class topic_manifest_version : int32_t {
    v1 = 1,
};

} // namespace cloud_storage
