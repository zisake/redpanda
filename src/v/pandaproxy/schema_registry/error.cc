/*
 * Copyright 2021 Vectorized, Inc.
 *
 * Use of this software is governed by the Business Source License
 * included in the file licenses/BSL.md
 *
 * As of the Change Date specified in that file, in accordance with
 * the Business Source License, use of this software will be governed
 * by the Apache License, Version 2.0
 */

#include "error.h"

#include "pandaproxy/error.h"
#include "pandaproxy/schema_registry/error.h"

namespace pandaproxy::schema_registry {

namespace {

struct error_category final : std::error_category {
    const char* name() const noexcept override {
        return "pandaproxy::schema_registry";
    }
    std::string message(int ev) const override {
        switch (static_cast<error_code>(ev)) {
        case error_code::schema_id_not_found:
            return "Schema not found";
        case error_code::schema_invalid:
            return "Invalid schema";
        case error_code::subject_not_found:
            return "Subject not found";
        case error_code::subject_version_not_found:
            return "Subject version not found";
        case error_code::subject_soft_deleted:
            return "Subject was soft deleted. Set permanent=true to delete "
                   "permanently";
        case error_code::subject_not_deleted:
            return "Subject not deleted before being permanently deleted";
        case error_code::subject_version_soft_deleted:
            return "Version was soft deleted. Set permanent=true to delete "
                   "permanently";
        case error_code::subject_version_not_deleted:
            return "Version not deleted before being permanently deleted";
        case error_code::topic_parse_error:
            return "Unexpected data found in topic";
        }
        return "(unrecognized error)";
    }
    // TODO(Ben): Determine how best to use default_error_condition between
    // pandaproxy/rest and pandaproxy/schema_registry
    std::error_condition
    default_error_condition(int ec) const noexcept override {
        switch (static_cast<error_code>(ec)) {
        case error_code::schema_id_not_found:
        case error_code::subject_not_found:
        case error_code::subject_version_not_found:
            return reply_error_code::topic_not_found; // 40401
        case error_code::subject_soft_deleted:
            return reply_error_code::subject_soft_deleted; // 40404
        case error_code::subject_not_deleted:
            return reply_error_code::subject_not_deleted; // 40405
        case error_code::subject_version_soft_deleted:
            return reply_error_code::subject_version_soft_deleted; // 40406
        case error_code::subject_version_not_deleted:
            return reply_error_code::subject_version_not_deleted; // 40407
        case error_code::schema_invalid:
            return reply_error_code::unprocessable_entity;
        case error_code::topic_parse_error:
            return reply_error_code::zookeeper_error; // 50001
        }
        return {};
    }
};

const error_category pps_error_category{};

}; // namespace

std::error_code make_error_code(error_code e) {
    return {static_cast<int>(e), pps_error_category};
}

} // namespace pandaproxy::schema_registry
