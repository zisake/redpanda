// Copyright 2020 Vectorized, Inc.
//
// Use of this software is governed by the Business Source License
// included in the file licenses/BSL.md
//
// As of the Change Date specified in that file, in accordance with
// the Business Source License, use of this software will be governed
// by the Apache License, Version 2.0

#include "cluster/scheduling/types.h"
#include "cluster/tests/partition_allocator_fixture.h"
#include "raft/types.h"
#include "random/fast_prng.h"
#include "random/generators.h"
#include "test_utils/fixture.h"

#include <seastar/core/sharded.hh>

#include <boost/test/tools/old/interface.hpp>

void validate_replica_set_diversity(
  const std::vector<cluster::partition_assignment> assignments) {
    for (const auto& assignment : assignments) {
        if (assignment.replicas.size() > 1) {
            auto sentinel = assignment.replicas.front();
            BOOST_TEST_REQUIRE(std::all_of(
              std::next(assignment.replicas.begin()),
              assignment.replicas.end(),
              [sentinel](const model::broker_shard bs) {
                  return sentinel.node_id != bs.node_id;
              }));
        }
    }
}

FIXTURE_TEST(register_node, partition_allocator_fixture) {
    register_node(0, 32);
    register_node(1, 64);
    register_node(2, 12);
    BOOST_REQUIRE(allocator.contains_node(model::node_id(0)));
    BOOST_REQUIRE(allocator.contains_node(model::node_id(1)));
    BOOST_REQUIRE(allocator.contains_node(model::node_id(2)));
    BOOST_REQUIRE_EQUAL(allocator.state().available_nodes(), 3);
}

FIXTURE_TEST(unregister_node, partition_allocator_fixture) {
    register_node(0, 32);
    register_node(1, 64);
    register_node(2, 12);
    allocator.unregister_node(model::node_id(1));
    BOOST_REQUIRE(allocator.contains_node(model::node_id(0)));
    BOOST_REQUIRE(!allocator.contains_node(model::node_id(1)));
    BOOST_REQUIRE(allocator.contains_node(model::node_id(2)));
    BOOST_REQUIRE_EQUAL(allocator.state().available_nodes(), 2);
}

FIXTURE_TEST(invalid_allocation_over_capacity, partition_allocator_fixture) {
    register_node(0, 8);
    register_node(1, 4);
    register_node(2, 6);

    saturate_all_machines();
    auto gr = allocator.state().last_group_id();
    BOOST_REQUIRE(
      allocator.allocate(make_allocation_request(1, 1)).has_error());
    // group id hasn't changed
    BOOST_REQUIRE_EQUAL(allocator.state().last_group_id(), gr);
}

FIXTURE_TEST(max_allocation, partition_allocator_fixture) {
    register_node(0, 2);
    register_node(1, 2);
    register_node(2, 2);
    // available capacity
    // 3 * 7000 * 2 - 3*2 = 41994

    auto req = make_allocation_request(max_capacity() / 3, 3);

    auto units = allocator.allocate(std::move(req)).value();

    BOOST_REQUIRE_EQUAL(units.get_assignments().size(), 13998);
    BOOST_REQUIRE_EQUAL(allocated_nodes_count(units.get_assignments()), 41994);
    BOOST_REQUIRE_EQUAL(allocator.state().last_group_id()(), 13998);
    validate_replica_set_diversity(units.get_assignments());

    // make sure there is no room left after
    auto single_partition_req = make_allocation_request(1, 1);
    auto result = allocator.allocate(std::move(single_partition_req));
    BOOST_REQUIRE(result.has_error());
}

FIXTURE_TEST(unsatisfyable_diversity_assignment, partition_allocator_fixture) {
    register_node(0, 8);
    register_node(1, 4);
    register_node(2, 6);

    auto req = make_allocation_request(1, 5);
    auto allocs = allocator.allocate(std::move(req));
    BOOST_TEST_REQUIRE(allocs.has_error());
    BOOST_REQUIRE_EQUAL(
      cluster::errc(allocs.error().value()),
      cluster::errc::topic_invalid_replication_factor);

    // ensure rollback happened
    BOOST_REQUIRE(all_nodes_empty());

    BOOST_REQUIRE_EQUAL(allocator.state().last_group_id()(), 0);
}
FIXTURE_TEST(partial_assignment, partition_allocator_fixture) {
    register_node(0, 2);
    register_node(1, 2);
    register_node(2, 2);
    auto max_partitions_in_cluster = max_capacity() / 3;

    auto units_1 = allocator
                     .allocate(make_allocation_request(
                       max_partitions_in_cluster - 1, 3))
                     .value();
    BOOST_REQUIRE_EQUAL(
      units_1.get_assignments().size(), max_partitions_in_cluster - 1);
    validate_replica_set_diversity(units_1.get_assignments());
    // allocate 2 partitions - one should fail, returning null & deallocating

    auto req_2 = make_allocation_request(2, 3);
    auto units_2 = allocator.allocate(std::move(req_2));
    BOOST_REQUIRE(units_2.has_error());

    BOOST_REQUIRE_EQUAL(3, max_capacity());
    BOOST_REQUIRE_EQUAL(
      allocator.state().last_group_id()(), max_partitions_in_cluster - 1);
}
FIXTURE_TEST(max_deallocation, partition_allocator_fixture) {
    register_node(0, 3);
    register_node(1, 3);
    register_node(2, 3);
    // This test performs - 209994 partition assignments
    const auto max = max_capacity();

    {
        cluster::allocation_units allocs
          = allocator.allocate(make_allocation_request(max / 3, 3)).value();

        BOOST_REQUIRE_EQUAL(allocs.get_assignments().size() * 3, max);

        BOOST_REQUIRE_EQUAL(allocator.state().last_group_id()(), max / 3);
    }

    BOOST_REQUIRE_EQUAL(allocator.state().last_group_id()(), max / 3);
    BOOST_REQUIRE_EQUAL(max_capacity(), max);
}

FIXTURE_TEST(recovery_test, partition_allocator_fixture) {
    register_node(0, 3);
    register_node(1, 3);
    register_node(2, 3);
    auto create_replicas = [this](int topics, int partitions) {
        std::vector<model::broker_shard> ret;

        for (int t = 0; t < topics; t++) {
            for (int p = 0; p < partitions; p++) {
                std::vector<model::broker_shard> replicas;
                for (int r = 0; r < 3; r++) {
                    ret.push_back(
                      model::broker_shard{model::node_id(r), prng() % 3});
                }
            }
        }
        return ret;
    };
    // 100 topics with 12 partitions each replicated on 3 nodes each
    auto replicas = create_replicas(100, 12);
    allocator.update_allocation_state(replicas, raft::group_id(0));
    // each node in the cluster holds one replica for each partition,
    // so it has to have topics * partitions shards allocated
    cluster::allocation_node::allocation_capacity allocated_shards{100 * 12};
    // Remaining capacity on node 0
    BOOST_REQUIRE_EQUAL(
      allocator.state()
        .allocation_nodes()
        .find(model::node_id(0))
        ->second->allocated_partitions(),
      allocated_shards);
    // Remaining capacity on node 1
    BOOST_REQUIRE_EQUAL(
      allocator.state()
        .allocation_nodes()
        .find(model::node_id(1))
        ->second->allocated_partitions(),
      allocated_shards);
    // Remaining capacity on node 2
    BOOST_REQUIRE_EQUAL(
      allocator.state()
        .allocation_nodes()
        .find(model::node_id(2))
        ->second->allocated_partitions(),
      allocated_shards);
}

FIXTURE_TEST(allocation_units_test, partition_allocator_fixture) {
    register_node(1, 10);
    register_node(2, 11);
    register_node(3, 12);
    // just fill up the cluster partially

    {
        auto allocs
          = allocator.allocate(make_allocation_request(10, 3)).value();
        BOOST_REQUIRE_EQUAL(allocs.get_assignments().size(), 10);
        BOOST_REQUIRE_EQUAL(
          allocated_nodes_count(allocs.get_assignments()), 3 * 10);
    }

    BOOST_REQUIRE(all_nodes_empty());

    // we do not decrement the highest raft group
    BOOST_REQUIRE_EQUAL(allocator.state().last_group_id()(), 10);
}
FIXTURE_TEST(decommission_node, partition_allocator_fixture) {
    register_node(0, 32);
    register_node(1, 64);
    register_node(2, 12);
    allocator.decommission_node(model::node_id(1));

    // only two of machines are available as one of them is decommissioned
    BOOST_REQUIRE_EQUAL(allocator.state().available_nodes(), 2);
}

FIXTURE_TEST(
  test_decommissioned_realloc_single_replica, partition_allocator_fixture) {
    register_node(0, 2);
    register_node(1, 4);
    register_node(2, 7);
    auto req = make_allocation_request(1, 1);
    cluster::partition_assignment previous_assignment;
    cluster::partition_assignment new_assignment;
    {
        auto allocs = allocator.allocate(std::move(req)).value();
        previous_assignment = allocs.get_assignments().front();

        allocator.update_allocation_state(
          allocs.get_assignments().front().replicas,
          allocs.get_assignments().front().group);
    }

    allocator.decommission_node(previous_assignment.replicas.front().node_id);
    BOOST_REQUIRE_EQUAL(allocator.state().allocation_nodes().size(), 3);
    // only two of machines are available as one of them is decommissioned
    BOOST_REQUIRE_EQUAL(allocator.state().available_nodes(), 2);

    register_node(10, 3);
    {
        auto reallocated = allocator.reassign_decommissioned_replicas(
          previous_assignment);
        //  second attempt should be successfull
        BOOST_REQUIRE_EQUAL(reallocated.has_value(), true);
        BOOST_REQUIRE_EQUAL(reallocated.value().get_assignments().size(), 1);
        BOOST_REQUIRE_EQUAL(
          reallocated.value().get_assignments().begin()->replicas.size(), 1);
        new_assignment = reallocated.value().get_assignments().front();
    }
    // update allocation state after units left scope
    allocator.update_allocation_state(
      new_assignment.replicas, previous_assignment.replicas);

    auto total_allocated = std::accumulate(
      allocator.state().allocation_nodes().begin(),
      allocator.state().allocation_nodes().end(),
      0,
      [](int acc, const auto& node) {
          return acc + node.second->allocated_partitions();
      });
    BOOST_REQUIRE_EQUAL(total_allocated, 1);
}

FIXTURE_TEST(test_decommissioned_realloc, partition_allocator_fixture) {
    register_node(0, 2);
    register_node(1, 4);
    register_node(2, 7);
    auto req = make_allocation_request(1, 3);

    cluster::partition_assignment previous_assignment;
    cluster::partition_assignment new_assignment;
    {
        auto allocs = allocator.allocate(std::move(req)).value();
        previous_assignment = allocs.get_assignments().front();
        allocator.update_allocation_state(
          allocs.get_assignments().front().replicas,
          allocs.get_assignments().front().group);
    }

    allocator.decommission_node(model::node_id(2));
    BOOST_REQUIRE_EQUAL(allocator.state().allocation_nodes().size(), 3);
    // only two of machines are available as one of them is decommissioned
    BOOST_REQUIRE_EQUAL(allocator.state().available_nodes(), 2);

    // reallocate
    auto first_attempt = allocator.reassign_decommissioned_replicas(
      previous_assignment);
    // first attempt should fail, there are not enough nodes to allocate
    // replicas (requested rf = 3, while we have 2 nodes)
    BOOST_REQUIRE_EQUAL(first_attempt.has_value(), false);

    register_node(10, 3);
    {
        auto second_attempt = allocator.reassign_decommissioned_replicas(
          previous_assignment);
        //  second attempt should be successfull
        BOOST_REQUIRE_EQUAL(second_attempt.has_value(), true);
        BOOST_REQUIRE_EQUAL(second_attempt.value().get_assignments().size(), 1);
        BOOST_REQUIRE_EQUAL(
          second_attempt.value().get_assignments().begin()->replicas.size(), 3);
        new_assignment = second_attempt.value().get_assignments().front();

        BOOST_REQUIRE_EQUAL(
          allocator.state()
            .allocation_nodes()
            .find(model::node_id(10))
            ->second->allocated_partitions(),
          cluster::allocation_node::allocation_capacity(1));
        BOOST_REQUIRE_EQUAL(
          allocator.state()
            .allocation_nodes()
            .find(model::node_id(0))
            ->second->allocated_partitions(),
          cluster::allocation_node::allocation_capacity(1));
        BOOST_REQUIRE_EQUAL(
          allocator.state()
            .allocation_nodes()
            .find(model::node_id(1))
            ->second->allocated_partitions(),
          cluster::allocation_node::allocation_capacity(1));
    }
    // update allocation state after units left scope
    allocator.update_allocation_state(
      new_assignment.replicas, previous_assignment.replicas);
    BOOST_REQUIRE_EQUAL(
      allocator.state()
        .allocation_nodes()
        .find(model::node_id(10))
        ->second->allocated_partitions(),
      cluster::allocation_node::allocation_capacity(1));
    BOOST_REQUIRE_EQUAL(
      allocator.state()
        .allocation_nodes()
        .find(model::node_id(0))
        ->second->allocated_partitions(),
      cluster::allocation_node::allocation_capacity(1));
    BOOST_REQUIRE_EQUAL(
      allocator.state()
        .allocation_nodes()
        .find(model::node_id(1))
        ->second->allocated_partitions(),
      cluster::allocation_node::allocation_capacity(1));
    BOOST_REQUIRE_EQUAL(
      allocator.state()
        .allocation_nodes()
        .find(model::node_id(2))
        ->second->allocated_partitions(),
      cluster::allocation_node::allocation_capacity(0));
}

cluster::hard_constraint_evaluator make_throwning_hard_evaluator() {
    struct impl : cluster::hard_constraint_evaluator::impl {
        bool evaluate(const cluster::allocation_node&) const final {
            throw std::runtime_error("evaluation exception");
        }
        void print(std::ostream& o) const final {
            fmt::print(o, "exception throwing hard constraint evaluator");
        }
    };

    return cluster::hard_constraint_evaluator(std::make_unique<impl>());
}

cluster::hard_constraint_evaluator make_false_evaluator() {
    struct impl : cluster::hard_constraint_evaluator::impl {
        bool evaluate(const cluster::allocation_node&) const final {
            return false;
        }
        void print(std::ostream& o) const final {
            fmt::print(o, "false returning constraint evaluator");
        }
    };

    return cluster::hard_constraint_evaluator(std::make_unique<impl>());
}

cluster::hard_constraint_evaluator make_nop_evaluator() {
    struct impl : cluster::hard_constraint_evaluator::impl {
        bool evaluate(const cluster::allocation_node&) const final {
            return true;
        }
        void print(std::ostream& o) const final {
            fmt::print(o, "false returning constraint evaluator");
        }
    };

    return cluster::hard_constraint_evaluator(std::make_unique<impl>());
}

cluster::hard_constraint_evaluator random_evaluator() {
    auto gen_id = random_generators::get_int(0, 2);
    switch (gen_id) {
    case 0:
        return make_throwning_hard_evaluator();
    case 1:
        return make_false_evaluator();
    default:
        return make_nop_evaluator();
    }
}

FIXTURE_TEST(allocator_exception_safety_test, partition_allocator_fixture) {
    register_node(0, 2);
    register_node(1, 4);
    register_node(2, 7);

    auto capacity = max_capacity();
    for (int i = 0; i < 500; ++i) {
        auto req = make_allocation_request(1, 1);
        req.partitions[0].constraints.hard_constraints.push_back(
          ss::make_lw_shared<cluster::hard_constraint_evaluator>(
            random_evaluator()));
        try {
            auto res = allocator.allocate(std::move(req));
            if (res) {
                capacity--;
                for (auto& as : res.value().get_assignments()) {
                    allocator.update_allocation_state(as.replicas, as.group);
                }
            }

        } catch (...) {
        }
        BOOST_REQUIRE_EQUAL(capacity, max_capacity());
    }
}
