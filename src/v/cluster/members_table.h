/*
 * Copyright 2020 Vectorized, Inc.
 *
 * Use of this software is governed by the Business Source License
 * included in the file licenses/BSL.md
 *
 * As of the Change Date specified in that file, in accordance with
 * the Business Source License, use of this software will be governed
 * by the Apache License, Version 2.0
 */

#pragma once

#include "cluster/commands.h"
#include "cluster/types.h"
#include "model/metadata.h"

#include <absl/container/flat_hash_map.h>

namespace cluster {

/// Class containing information about cluster members. The members class is
/// instantiated on each core. Cluster members updates are comming directly from
/// cluster::members_manager
class members_table {
public:
    using broker_ptr = ss::lw_shared_ptr<model::broker>;

    std::vector<broker_ptr> all_brokers() const;

    std::vector<model::node_id> all_broker_ids() const;

    /// Returns single broker if exists in cache
    std::optional<broker_ptr> get_broker(model::node_id) const;

    std::vector<model::node_id> get_decommissioned() const;

    bool contains(model::node_id) const;

    void update_brokers(patch<broker_ptr>);

    std::error_code apply(decommission_node_cmd);
    std::error_code apply(recommission_node_cmd);

private:
    using broker_cache_t = absl::flat_hash_map<model::node_id, broker_ptr>;
    broker_cache_t _brokers;
};
} // namespace cluster
