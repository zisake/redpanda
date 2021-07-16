// Copyright 2020 Vectorized, Inc.
//
// Use of this software is governed by the Business Source License
// included in the file licenses/BSL.md
//
// As of the Change Date specified in that file, in accordance with
// the Business Source License, use of this software will be governed
// by the Apache License, Version 2.0

#include "handlers.h"

#include "kafka/client/exceptions.h"
#include "kafka/protocol/errors.h"
#include "kafka/protocol/fetch.h"
#include "kafka/protocol/leave_group.h"
#include "kafka/protocol/offset_commit.h"
#include "kafka/protocol/offset_fetch.h"
#include "kafka/protocol/schemata/offset_commit_request.h"
#include "kafka/types.h"
#include "model/fundamental.h"
#include "pandaproxy/json/exceptions.h"
#include "pandaproxy/json/requests/create_consumer.h"
#include "pandaproxy/json/requests/fetch.h"
#include "pandaproxy/json/requests/offset_commit.h"
#include "pandaproxy/json/requests/offset_fetch.h"
#include "pandaproxy/json/requests/partition_offsets.h"
#include "pandaproxy/json/requests/partitions.h"
#include "pandaproxy/json/requests/produce.h"
#include "pandaproxy/json/requests/subscribe_consumer.h"
#include "pandaproxy/json/rjson_util.h"
#include "pandaproxy/parsing/httpd.h"
#include "pandaproxy/reply.h"
#include "pandaproxy/rest/configuration.h"
#include "raft/types.h"
#include "ssx/future-util.h"
#include "ssx/sformat.h"
#include "storage/record_batch_builder.h"

#include <seastar/core/coroutine.hh>
#include <seastar/core/future.hh>
#include <seastar/core/std-coroutine.hh>
#include <seastar/http/reply.hh>

#include <absl/container/flat_hash_map.h>
#include <boost/algorithm/string.hpp>
#include <boost/lexical_cast.hpp>

#include <algorithm>
#include <chrono>

namespace ppj = pandaproxy::json;

namespace pandaproxy::rest {

namespace {

using server = ctx_server<proxy>;

ss::shard_id consumer_shard(const kafka::group_id& g_id) {
    auto hash = xxhash_64(g_id().data(), g_id().length());
    return jump_consistent_hash(hash, ss::smp::count);
}

} // namespace

ss::future<server::reply_t>
get_topics_names(server::request_t rq, server::reply_t rp) {
    parse::content_type_header(
      *rq.req,
      {json::serialization_format::v2, json::serialization_format::none});
    auto res_fmt = parse::accept_header(
      *rq.req,
      {json::serialization_format::v2, json::serialization_format::none});
    rq.req.reset();
    auto make_list_topics_req = []() {
        return kafka::metadata_request{.list_all_topics = true};
    };
    return rq.service()
      .client()
      .local()
      .dispatch(make_list_topics_req)
      .then([res_fmt, rp = std::move(rp)](
              kafka::metadata_request::api_type::response_type res) mutable {
          std::vector<model::topic_view> names;
          names.reserve(res.data.topics.size());

          std::transform(
            res.data.topics.begin(),
            res.data.topics.end(),
            std::back_inserter(names),
            [](const kafka::metadata_response::topic& e) {
                return model::topic_view(e.name);
            });

          auto json_rslt = ppj::rjson_serialize(names);
          rp.rep->write_body("json", json_rslt);
          rp.mime_type = res_fmt;
          return std::move(rp);
      });
}

ss::future<server::reply_t>
get_topics_records(server::request_t rq, server::reply_t rp) {
    parse::content_type_header(
      *rq.req,
      {json::serialization_format::v2, json::serialization_format::none});
    auto res_fmt = parse::accept_header(
      *rq.req,
      {json::serialization_format::binary_v2,
       json::serialization_format::json_v2});

    auto tp{model::topic_partition{
      parse::request_param<model::topic>(*rq.req, "topic_name"),
      parse::request_param<model::partition_id>(*rq.req, "partition_id")}};
    auto offset{parse::query_param<model::offset>(*rq.req, "offset")};
    auto timeout{
      parse::query_param<std::chrono::milliseconds>(*rq.req, "timeout")};
    int32_t max_bytes{parse::query_param<int32_t>(*rq.req, "max_bytes")};

    rq.req.reset();
    return rq.service()
      .client()
      .local()
      .fetch_partition(std::move(tp), offset, max_bytes, timeout)
      .then([res_fmt, rp = std::move(rp)](kafka::fetch_response res) mutable {
          rapidjson::StringBuffer str_buf;
          rapidjson::Writer<rapidjson::StringBuffer> w(str_buf);

          ppj::rjson_serialize_fmt(res_fmt)(w, std::move(res));

          // TODO Ben: Prevent this linearization
          ss::sstring json_rslt = str_buf.GetString();
          rp.rep->write_body("json", json_rslt);
          rp.mime_type = res_fmt;
          return std::move(rp);
      });
}

ss::future<server::reply_t>
post_topics_name(server::request_t rq, server::reply_t rp) {
    auto req_fmt = parse::content_type_header(
      *rq.req,
      {json::serialization_format::binary_v2,
       json::serialization_format::json_v2});
    auto res_fmt = parse::accept_header(
      *rq.req,
      {json::serialization_format::v2, json::serialization_format::none});

    auto topic = parse::request_param<model::topic>(*rq.req, "topic_name");

    auto records = ppj::rjson_parse(
      rq.req->content.data(), ppj::produce_request_handler(req_fmt));

    auto res = co_await rq.service().client().local().produce_records(
      topic, std::move(records));

    auto json_rslt = ppj::rjson_serialize(res.data.responses[0]);
    rp.rep->write_body("json", json_rslt);
    rp.mime_type = res_fmt;
    co_return rp;
}

static ss::sstring make_consumer_uri(
  const server::request_t& request,
  const kafka::member_id& m_id,
  const kafka::group_id& group_id) {
    auto& addr = request.ctx.advertised_listeners[request.req->listener_idx];
    return ssx::sformat(
      "{}://{}:{}/consumers/{}/instances/{}",
      request.req->get_protocol_name(),
      addr.host(),
      addr.port(),
      group_id(),
      m_id());
}

ss::future<server::reply_t>
create_consumer(server::request_t rq, server::reply_t rp) {
    parse::content_type_header(*rq.req, {json::serialization_format::v2});
    auto res_fmt = parse::accept_header(
      *rq.req,
      {json::serialization_format::v2, json::serialization_format::none});

    auto group_id = parse::request_param<kafka::group_id>(
      *rq.req, "group_name");

    auto req_data = ppj::rjson_parse(
      rq.req->content.data(), ppj::create_consumer_request_handler());

    if (req_data.format != "binary" && req_data.format != "json") {
        throw parse::error(
          parse::error_code::invalid_param,
          "format must be 'binary' or 'json'");
    }
    if (req_data.auto_offset_reset != "earliest") {
        throw parse::error(
          parse::error_code::invalid_param, "auto.offset must be earliest");
    }
    if (req_data.auto_commit_enable != "false") {
        throw parse::error(
          parse::error_code::invalid_param, "auto.commit must be false");
    }

    auto handler =
      [group_id,
       res_fmt,
       req_data{std::move(req_data)},
       rq{std::move(rq)},
       rp{std::move(rp)}](
        kafka::client::client& client) mutable -> ss::future<server::reply_t> {
        auto name = co_await client.create_consumer(group_id, req_data.name);
        json::create_consumer_response res{
          .instance_id = name,
          .base_uri = make_consumer_uri(rq, name, group_id)};
        auto json_rslt = ppj::rjson_serialize(res);
        rp.rep->write_body("json", json_rslt);
        rp.mime_type = res_fmt;
        co_return std::move(rp);
    };

    co_return co_await rq.service().client().invoke_on(
      consumer_shard(group_id), rq.context().smp_sg, std::move(handler));
}

ss::future<server::reply_t>
remove_consumer(server::request_t rq, server::reply_t rp) {
    parse::content_type_header(*rq.req, {json::serialization_format::v2});
    parse::accept_header(
      *rq.req,
      {json::serialization_format::v2, json::serialization_format::none});

    auto group_id = parse::request_param<kafka::group_id>(
      *rq.req, "group_name");
    auto member_id = parse::request_param<kafka::member_id>(
      *rq.req, "instance");

    auto handler =
      [group_id, member_id, rq{std::move(rq)}, rp{std::move(rp)}](
        kafka::client::client& client) mutable -> ss::future<server::reply_t> {
        co_await client.remove_consumer(group_id, member_id);
        rp.rep->set_status(ss::httpd::reply::status_type::no_content);
        co_return std::move(rp);
    };

    co_return co_await rq.service().client().invoke_on(
      consumer_shard(group_id), rq.context().smp_sg, std::move(handler));
}

ss::future<server::reply_t>
subscribe_consumer(server::request_t rq, server::reply_t rp) {
    parse::content_type_header(*rq.req, {json::serialization_format::v2});
    auto res_fmt = parse::accept_header(
      *rq.req,
      {json::serialization_format::v2, json::serialization_format::none});

    auto group_id = parse::request_param<kafka::group_id>(
      *rq.req, "group_name");
    auto member_id = parse::request_param<kafka::member_id>(
      *rq.req, "instance");

    auto req_data = ppj::rjson_parse(
      rq.req->content.data(), ppj::subscribe_consumer_request_handler());

    auto handler =
      [group_id,
       member_id,
       res_fmt,
       req_data{std::move(req_data)},
       rq{std::move(rq)},
       rp{std::move(rp)}](
        kafka::client::client& client) mutable -> ss::future<server::reply_t> {
        co_await client.subscribe_consumer(
          group_id, member_id, std::move(req_data.topics));
        rp.mime_type = res_fmt;
        rp.rep->set_status(ss::httpd::reply::status_type::no_content);
        co_return std::move(rp);
    };

    co_return co_await rq.service().client().invoke_on(
      consumer_shard(group_id), rq.context().smp_sg, std::move(handler));
}

ss::future<server::reply_t>
consumer_fetch(server::request_t rq, server::reply_t rp) {
    parse::content_type_header(
      *rq.req,
      {json::serialization_format::v2, json::serialization_format::none});
    auto res_fmt = parse::accept_header(
      *rq.req,
      {json::serialization_format::binary_v2,
       json::serialization_format::json_v2});

    auto group_id{parse::request_param<kafka::group_id>(*rq.req, "group_name")};
    auto name{parse::request_param<kafka::member_id>(*rq.req, "instance")};
    auto timeout{parse::query_param<std::optional<std::chrono::milliseconds>>(
      *rq.req, "timeout")};
    auto max_bytes{
      parse::query_param<std::optional<int32_t>>(*rq.req, "max_bytes")};

    auto handler =
      [group_id,
       name,
       timeout,
       max_bytes,
       res_fmt,
       rq{std::move(rq)},
       rp{std::move(rp)}](
        kafka::client::client& client) mutable -> ss::future<server::reply_t> {
        auto res = co_await client.consumer_fetch(
          group_id, name, timeout, max_bytes);
        rapidjson::StringBuffer str_buf;
        rapidjson::Writer<rapidjson::StringBuffer> w(str_buf);

        ppj::rjson_serialize_fmt(res_fmt)(w, std::move(res));

        // TODO Ben: Prevent this linearization
        ss::sstring json_rslt = str_buf.GetString();
        rp.rep->write_body("json", json_rslt);
        rp.mime_type = res_fmt;
        co_return std::move(rp);
    };

    co_return co_await rq.service().client().invoke_on(
      consumer_shard(group_id), rq.context().smp_sg, std::move(handler));
}

ss::future<server::reply_t>
get_consumer_offsets(server::request_t rq, server::reply_t rp) {
    parse::content_type_header(*rq.req, {json::serialization_format::v2});
    auto res_fmt = parse::accept_header(
      *rq.req,
      {json::serialization_format::v2, json::serialization_format::none});

    auto group_id{parse::request_param<kafka::group_id>(*rq.req, "group_name")};
    auto member_id{parse::request_param<kafka::member_id>(*rq.req, "instance")};

    auto req_data = ppj::partitions_request_to_offset_request(ppj::rjson_parse(
      rq.req->content.data(), ppj::partitions_request_handler()));

    auto handler =
      [group_id,
       member_id,
       req_data{std::move(req_data)},
       res_fmt,
       rq{std::move(rq)},
       rp{std::move(rp)}](
        kafka::client::client& client) mutable -> ss::future<server::reply_t> {
        auto res = co_await client.consumer_offset_fetch(
          group_id, member_id, std::move(req_data));
        rapidjson::StringBuffer str_buf;
        rapidjson::Writer<rapidjson::StringBuffer> w(str_buf);
        ppj::rjson_serialize(w, res);
        ss::sstring json_rslt = str_buf.GetString();
        rp.rep->write_body("json", json_rslt);
        rp.mime_type = res_fmt;
        co_return std::move(rp);
    };

    co_return co_await rq.service().client().invoke_on(
      consumer_shard(group_id), rq.context().smp_sg, std::move(handler));
}

ss::future<server::reply_t>
post_consumer_offsets(server::request_t rq, server::reply_t rp) {
    parse::content_type_header(*rq.req, {json::serialization_format::v2});
    parse::accept_header(
      *rq.req,
      {json::serialization_format::v2, json::serialization_format::none});

    auto group_id{parse::request_param<kafka::group_id>(*rq.req, "group_name")};
    auto member_id{parse::request_param<kafka::member_id>(*rq.req, "instance")};

    // If the request is empty, commit all offsets
    auto req_data = rq.req->content.length() == 0
                      ? std::vector<kafka::offset_commit_request_topic>()
                      : ppj::partition_offsets_request_to_offset_commit_request(
                        ppj::rjson_parse(
                          rq.req->content.data(),
                          ppj::partition_offsets_request_handler()));

    auto handler =
      [group_id,
       member_id,
       req_data{std::move(req_data)},
       rq{std::move(rq)},
       rp{std::move(rp)}](
        kafka::client::client& client) mutable -> ss::future<server::reply_t> {
        auto res = co_await client.consumer_offset_commit(
          group_id, member_id, std::move(req_data));
        rp.rep->set_status(ss::httpd::reply::status_type::no_content);
        co_return std::move(rp);
    };

    co_return co_await rq.service().client().invoke_on(
      consumer_shard(group_id), rq.context().smp_sg, std::move(handler));
}

} // namespace pandaproxy::rest
