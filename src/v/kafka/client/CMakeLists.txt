v_cc_library(
  NAME kafka_client
  SRCS
    assignment_plans.cc
    broker.cc
    brokers.cc
    client.cc
    configuration.cc
    consumer.cc
    fetcher.cc
    fetch_session.cc
    partitioners.cc
    producer.cc
    topic_cache.cc
  DEPS
    v::kafka
    v::ssx
  )

add_subdirectory(test)
