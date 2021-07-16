/*
 * Copyright 2021 Vectorized, Inc.
 *
 * Licensed as a Redpanda Enterprise file under the Redpanda Community
 * License (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 *
 * https://github.com/vectorizedio/redpanda/blob/master/licenses/rcl.md
 */

#include "coproc/tests/fixtures/coproc_slim_fixture.h"
#include "coproc/tests/utils/coprocessor.h"
#include "model/namespace.h"
#include "storage/tests/utils/random_batch.h"
#include "test_utils/fixture.h"

#include <boost/test/tools/old/interface.hpp>
#include <boost/test/unit_test_log.hpp>

#include <chrono>

class tip_fixture : public coproc_slim_fixture {
public:
    std::optional<std::size_t> run(
      coproc::topic_ingestion_policy tip, std::size_t n, std::size_t drain_n) {
        model::topic infoo("infoo");
        model::ntp infoo_ntp(
          model::kafka_namespace, infoo, model::partition_id(0));
        setup({{infoo, 1}}).get();

        push(
          infoo_ntp,
          storage::test::make_random_memory_record_batch_reader(
            model::offset(0), n, 1))
          .get();

        /// Using the slim fixture makes this test possible, otherwise the call
        /// to .get() here just ensures the coprocessor was pushed into the
        /// copro topic, not fully registered
        enable_coprocessors(
          {{.id = 78,
            .data{
              .tid = coproc::registry::type_identifier::identity_coprocessor,
              .topics = {{infoo, tip}}}}})
          .get();

        push(
          infoo_ntp,
          storage::test::make_random_memory_record_batch_reader(
            model::offset{0}, n, 1))
          .get();

        /// Assert that only the records from the second batch exist in the
        /// materialized log since the latest policy was chosen and the
        /// coprocessor was deployed between both pushes
        model::ntp output_ntp(
          model::kafka_namespace,
          model::to_materialized_topic(
            infoo, identity_coprocessor::identity_topic),
          model::partition_id(0));
        auto r = drain(output_ntp, drain_n).get();
        return !r.has_value() ? std::nullopt
                              : std::optional<std::size_t>(r->size());
    }
};

/// 'tip' stands for topic_ingestion_policy
FIXTURE_TEST(test_copro_tip_latest, tip_fixture) {
    auto result = run(tp_latest, 40, 40);
    BOOST_CHECK(result);
    BOOST_CHECK_EQUAL(*result, 40);
}

FIXTURE_TEST(test_copro_tip_earliest, tip_fixture) {
    auto result = run(tp_earliest, 40, 80);
    BOOST_CHECK(result);
    BOOST_CHECK_EQUAL(*result, 80);
}

FIXTURE_TEST(test_copro_tip_stored, coproc_slim_fixture) {
    model::topic sttp("sttp");
    model::ntp sttp_ntp(model::kafka_namespace, sttp, model::partition_id(0));
    setup({{sttp, 1}}).get();

    enable_coprocessors(
      {{.id = 7843,
        .data{
          .tid = coproc::registry::type_identifier::identity_coprocessor,
          .topics = {{sttp, tp_stored}}}}})
      .get();

    push(
      sttp_ntp,
      storage::test::make_random_memory_record_batch_reader(
        model::offset{0}, 40, 1))
      .get();

    ss::sleep(1s).get();
    info("Restarting....");
    restart().get();

    push(
      sttp_ntp,
      storage::test::make_random_memory_record_batch_reader(
        model::offset{0}, 40, 1))
      .get();

    /// Assert that only the records from the second batch exist in the
    /// materialized log since the latest policy was chosen and the coprocessor
    /// was deployed between both pushes
    model::ntp output_ntp(
      model::kafka_namespace,
      model::to_materialized_topic(sttp, identity_coprocessor::identity_topic),
      model::partition_id(0));
    auto results = drain(output_ntp, 80).get();
    BOOST_CHECK(results);
    BOOST_CHECK(results->size() >= 80);
}
