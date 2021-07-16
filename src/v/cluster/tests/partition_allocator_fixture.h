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

#include "cluster/scheduling/allocation_node.h"
#include "cluster/scheduling/allocation_strategy.h"
#include "cluster/scheduling/partition_allocator.h"
#include "model/fundamental.h"
#include "random/fast_prng.h"
#include "random/generators.h"

struct partition_allocator_fixture {
    void register_node(int id, int core_count) {
        allocator.register_node(std::make_unique<cluster::allocation_node>(
          model::node_id(id),
          core_count,
          absl::node_hash_map<ss::sstring, ss::sstring>{}));
    }

    void saturate_all_machines() {
        auto units = allocator.allocate(
          make_allocation_request(max_capacity(), 1));

        for (auto& pas : units.value().get_assignments()) {
            allocator.state().apply_update(pas.replicas, pas.group);
        }
    }

    uint allocated_nodes_count(
      const std::vector<cluster::partition_assignment>& allocs) {
        return std::accumulate(
          allocs.begin(),
          allocs.end(),
          0U,
          [](size_t acc, const cluster::partition_assignment& p_as) {
              return acc + p_as.replicas.size();
          });
    }

    bool all_nodes_empty() {
        return std::all_of(
          allocator.state().allocation_nodes().begin(),
          allocator.state().allocation_nodes().end(),
          [](const auto& n) { return n.second->empty(); });
    }

    int32_t max_capacity() {
        return std::accumulate(
          allocator.state().allocation_nodes().begin(),
          allocator.state().allocation_nodes().end(),
          0,
          [](int acc, auto& n) {
              return acc + n.second->partition_capacity();
          });
    }

    cluster::allocation_request
    make_allocation_request(int partitions, uint16_t replication_factor) {
        cluster::allocation_request req;
        req.partitions.reserve(partitions);
        for (int i = 0; i < partitions; ++i) {
            req.partitions.emplace_back(
              model::partition_id(i), replication_factor);
        }
        return req;
    }

    cluster::partition_allocator allocator;

    fast_prng prng;
};
