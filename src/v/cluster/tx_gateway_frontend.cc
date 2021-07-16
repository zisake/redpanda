// Copyright 2020 Vectorized, Inc.
//
// Use of this software is governed by the Business Source License
// included in the file licenses/BSL.md
//
// As of the Change Date specified in that file, in accordance with
// the Business Source License, use of this software will be governed
// by the Apache License, Version 2.0

#include "cluster/tx_gateway_frontend.h"

#include "cluster/id_allocator_frontend.h"
#include "cluster/logger.h"
#include "cluster/partition_leaders_table.h"
#include "cluster/partition_manager.h"
#include "cluster/rm_partition_frontend.h"
#include "cluster/shard_table.h"
#include "errc.h"
#include "types.h"

#include <seastar/core/coroutine.hh>

#include <algorithm>

namespace cluster {
using namespace std::chrono_literals;

static ss::future<bool> sleep_abortable(std::chrono::milliseconds dur) {
    try {
        co_await ss::sleep_abortable(dur);
        co_return true;
    } catch (const ss::sleep_aborted&) {
        co_return false;
    }
}

static add_paritions_tx_reply make_add_partitions_error_response(
  add_paritions_tx_request request, tx_errc ec) {
    add_paritions_tx_reply response;
    response.results.reserve(request.topics.size());
    for (auto& req_topic : request.topics) {
        add_paritions_tx_reply::topic_result res_topic;
        res_topic.name = req_topic.name;
        res_topic.results.reserve(req_topic.partitions.size());
        for (model::partition_id req_partition : req_topic.partitions) {
            add_paritions_tx_reply::partition_result res_partition;
            res_partition.partition_index = req_partition;
            res_partition.error_code = ec;
            res_topic.results.push_back(res_partition);
        }
        response.results.push_back(res_topic);
    }
    return response;
}

tx_gateway_frontend::tx_gateway_frontend(
  ss::smp_service_group ssg,
  ss::sharded<cluster::partition_manager>& partition_manager,
  ss::sharded<cluster::shard_table>& shard_table,
  ss::sharded<cluster::metadata_cache>& metadata_cache,
  ss::sharded<rpc::connection_cache>& connection_cache,
  ss::sharded<partition_leaders_table>& leaders,
  cluster::controller* controller,
  ss::sharded<cluster::id_allocator_frontend>& id_allocator_frontend,
  rm_group_proxy* group_proxy,
  ss::sharded<cluster::rm_partition_frontend>& rm_partition_frontend)
  : _ssg(ssg)
  , _partition_manager(partition_manager)
  , _shard_table(shard_table)
  , _metadata_cache(metadata_cache)
  , _connection_cache(connection_cache)
  , _leaders(leaders)
  , _controller(controller)
  , _id_allocator_frontend(id_allocator_frontend)
  , _rm_group_proxy(group_proxy)
  , _rm_partition_frontend(rm_partition_frontend)
  , _metadata_dissemination_retries(
      config::shard_local_cfg().metadata_dissemination_retries.value())
  , _metadata_dissemination_retry_delay_ms(
      config::shard_local_cfg().metadata_dissemination_retry_delay_ms.value()) {
}

ss::future<> tx_gateway_frontend::stop() { return _gate.close(); }

ss::future<std::optional<model::node_id>> tx_gateway_frontend::get_tx_broker() {
    auto has_topic = ss::make_ready_future<bool>(true);

    if (!_metadata_cache.local().contains(
          model::tx_manager_nt, model::tx_manager_ntp.tp.partition)) {
        has_topic = try_create_tx_topic();
    }

    auto timeout = ss::lowres_clock::now()
                   + config::shard_local_cfg().wait_for_leader_timeout_ms();

    return has_topic.then([this, timeout](bool does_topic_exist) {
        if (!does_topic_exist) {
            return ss::make_ready_future<std::optional<model::node_id>>(
              std::nullopt);
        }

        auto md = _metadata_cache.local().get_topic_metadata(
          model::tx_manager_nt);
        if (!md) {
            return ss::make_ready_future<std::optional<model::node_id>>(
              std::nullopt);
        }
        return _metadata_cache.local()
          .get_leader(model::tx_manager_ntp, timeout)
          .then([](model::node_id leader) {
              return std::optional<model::node_id>(leader);
          })
          .handle_exception([](std::exception_ptr e) {
              vlog(
                clusterlog.warn,
                "can't find find a leader of tx manager's topic {}",
                e);
              return ss::make_ready_future<std::optional<model::node_id>>(
                std::nullopt);
          });
    });
}

ss::future<try_abort_reply> tx_gateway_frontend::try_abort(
  model::partition_id tm,
  model::producer_identity pid,
  model::tx_seq tx_seq,
  model::timeout_clock::duration timeout) {
    if (!_metadata_cache.local().contains(
          model::tx_manager_nt, model::tx_manager_ntp.tp.partition)) {
        vlog(
          clusterlog.warn, "can't find {}/0 partition", model::tx_manager_nt);
        co_return try_abort_reply{.ec = tx_errc::partition_not_exists};
    }

    auto leader_opt = _leaders.local().get_leader(model::tx_manager_ntp);

    auto retries = _metadata_dissemination_retries;
    auto delay_ms = _metadata_dissemination_retry_delay_ms;
    auto aborted = false;
    while (!aborted && !leader_opt && 0 < retries--) {
        aborted = !co_await sleep_abortable(delay_ms);
        leader_opt = _leaders.local().get_leader(model::tx_manager_ntp);
    }

    if (!leader_opt) {
        vlog(
          clusterlog.warn, "can't find a leader for {}", model::tx_manager_ntp);
        co_return try_abort_reply{.ec = tx_errc::leader_not_found};
    }

    auto leader = leader_opt.value();
    auto _self = _controller->self();

    if (leader == _self) {
        co_return co_await try_abort_locally(tm, pid, tx_seq, timeout);
    }

    vlog(
      clusterlog.trace, "dispatching try_abort to {} from {}", leader, _self);

    co_return co_await dispatch_try_abort(leader, tm, pid, tx_seq, timeout);
}

ss::future<try_abort_reply> tx_gateway_frontend::try_abort_locally(
  model::partition_id tm,
  model::producer_identity pid,
  model::tx_seq tx_seq,
  model::timeout_clock::duration timeout) {
    auto shard = _shard_table.local().shard_for(model::tx_manager_ntp);

    auto retries = _metadata_dissemination_retries;
    auto delay_ms = _metadata_dissemination_retry_delay_ms;
    auto aborted = false;
    while (!aborted && !shard && 0 < retries--) {
        aborted = !co_await sleep_abortable(delay_ms);
        shard = _shard_table.local().shard_for(model::tx_manager_ntp);
    }

    if (!shard) {
        vlog(
          clusterlog.warn, "can't find a shard for {}", model::tx_manager_ntp);
        co_return try_abort_reply{.ec = tx_errc::shard_not_found};
    }

    co_return co_await do_try_abort(*shard, tm, pid, tx_seq, timeout);
}

ss::future<try_abort_reply> tx_gateway_frontend::dispatch_try_abort(
  model::node_id leader,
  model::partition_id tm,
  model::producer_identity pid,
  model::tx_seq tx_seq,
  model::timeout_clock::duration timeout) {
    return _connection_cache.local()
      .with_node_client<tx_gateway_client_protocol>(
        _controller->self(),
        ss::this_shard_id(),
        leader,
        timeout,
        [tm, pid, tx_seq, timeout](tx_gateway_client_protocol cp) {
            return cp.try_abort(
              try_abort_request{
                .tm = tm, .pid = pid, .tx_seq = tx_seq, .timeout = timeout},
              rpc::client_opts(model::timeout_clock::now() + timeout));
        })
      .then(&rpc::get_ctx_data<try_abort_reply>)
      .then([](result<try_abort_reply> r) {
          if (r.has_error()) {
              vlog(
                clusterlog.warn,
                "got error {} on remote init tm tx",
                r.error());
              return try_abort_reply{.ec = tx_errc::unknown_server_error};
          }

          return r.value();
      });
}

ss::future<try_abort_reply> tx_gateway_frontend::do_try_abort(
  ss::shard_id shard,
  model::partition_id,
  model::producer_identity pid,
  model::tx_seq tx_seq,
  model::timeout_clock::duration timeout) {
    return container().invoke_on(
      shard, _ssg, [pid, tx_seq, timeout](tx_gateway_frontend& self) {
          auto partition = self._partition_manager.local().get(
            model::tx_manager_ntp);
          if (!partition) {
              vlog(
                clusterlog.warn,
                "can't get partition by {} ntp",
                model::tx_manager_ntp);
              return ss::make_ready_future<try_abort_reply>(
                try_abort_reply{.ec = tx_errc::partition_not_found});
          }

          auto stm = partition->tm_stm();

          if (!stm) {
              vlog(
                clusterlog.warn,
                "can't get tm stm of the {}' partition",
                model::tx_manager_ntp);
              return ss::make_ready_future<try_abort_reply>(
                try_abort_reply{.ec = tx_errc::stm_not_found});
          }

          return stm->barrier().then([&self, stm, pid, tx_seq, timeout](
                                       bool ready) {
              if (!ready) {
                  return ss::make_ready_future<try_abort_reply>(
                    try_abort_reply{.ec = tx_errc::unknown_server_error});
              }
              auto tx_id_opt = stm->get_id_by_pid(pid);
              if (!tx_id_opt) {
                  return ss::make_ready_future<try_abort_reply>(
                    try_abort_reply{.aborted = true, .ec = tx_errc::none});
              }
              auto tx_id = tx_id_opt.value();
              return stm->get_tx_lock(tx_id)->with(
                [&self, stm, tx_id, pid, tx_seq, timeout]() {
                    return self.do_try_abort(stm, tx_id, pid, tx_seq, timeout);
                });
          });
      });
}

ss::future<try_abort_reply> tx_gateway_frontend::do_try_abort(
  ss::shared_ptr<tm_stm> stm,
  kafka::transactional_id transactional_id,
  model::producer_identity pid,
  model::tx_seq tx_seq,
  model::timeout_clock::duration timeout) {
    auto maybe_tx = co_await stm->get_actual_tx(transactional_id);
    if (!maybe_tx) {
        // unknown tx => state was lost => can't be comitted => aborted
        co_return try_abort_reply{.aborted = true, .ec = tx_errc::none};
    }

    auto tx = maybe_tx.value();

    if (tx.pid != pid) {
        // well, that's wierd, it may happen when the coordinator ended
        // a transaction and initiated a new session for the same tx.id
        // while the `try_abort` request was inflight
        co_return try_abort_reply{.ec = tx_errc::request_rejected};
    }

    if (tx.tx_seq != tx_seq) {
        // well, that's wierd, it may happen when the coordinator ended
        // a transaction and started a new one when the `try_abort`
        // request was inflight
        co_return try_abort_reply{.ec = tx_errc::request_rejected};
    }

    if (tx.status == tm_transaction::tx_status::prepared) {
        co_return try_abort_reply{.commited = true, .ec = tx_errc::none};
    } else if (
      tx.status == tm_transaction::tx_status::aborting
      || tx.status == tm_transaction::tx_status::killed
      || tx.status == tm_transaction::tx_status::ready) {
        // when it's ready it means in-memory state was lost
        // so can't be comitted and it's save to aborted
        co_return try_abort_reply{.aborted = true, .ec = tx_errc::none};
    } else if (tx.status == tm_transaction::tx_status::preparing) {
        (void)ss::with_gate(_gate, [this, stm, tx, timeout] {
            return stm->get_tx_lock(tx.id)->with([this, stm, tx, timeout]() {
                return do_commit_tm_tx(stm, tx.id, tx.pid, tx.tx_seq, timeout);
            });
        });
        co_return try_abort_reply{.ec = tx_errc::none};
    } else if (tx.status == tm_transaction::tx_status::ongoing) {
        auto killed_tx = co_await stm->try_change_status(
          tx.id, cluster::tm_transaction::tx_status::killed);
        if (!killed_tx.has_value()) {
            co_return try_abort_reply{.ec = tx_errc::unknown_server_error};
        }
        co_return try_abort_reply{.aborted = true, .ec = tx_errc::none};
    } else {
        vlog(clusterlog.error, "unknown tx status: {}", tx.status);
        co_return try_abort_reply{.ec = tx_errc::unknown_server_error};
    }
}

ss::future<checked<cluster::tm_transaction, tx_errc>>
tx_gateway_frontend::do_commit_tm_tx(
  ss::shared_ptr<cluster::tm_stm> stm,
  kafka::transactional_id tx_id,
  model::producer_identity pid,
  model::tx_seq tx_seq,
  model::timeout_clock::duration timeout) {
    auto maybe_tx = co_await stm->get_actual_tx(tx_id);
    if (!maybe_tx) {
        co_return tx_errc::request_rejected;
    }
    auto tx = maybe_tx.value();
    if (tx.pid != pid) {
        co_return tx_errc::request_rejected;
    }
    if (tx.tx_seq != tx_seq) {
        co_return tx_errc::request_rejected;
    }
    if (tx.status != tm_transaction::tx_status::preparing) {
        co_return tx_errc::request_rejected;
    }
    co_return co_await do_commit_tm_tx(
      stm, tx, timeout, ss::make_lw_shared<available_promise<tx_errc>>());
}

ss::future<cluster::init_tm_tx_reply> tx_gateway_frontend::init_tm_tx(
  kafka::transactional_id tx_id,
  std::chrono::milliseconds transaction_timeout_ms,
  model::timeout_clock::duration timeout) {
    if (!_metadata_cache.local().contains(
          model::tx_manager_nt, model::tx_manager_ntp.tp.partition)) {
        vlog(
          clusterlog.warn, "can't find {}/0 partition", model::tx_manager_nt);
        co_return cluster::init_tm_tx_reply{
          .ec = tx_errc::partition_not_exists};
    }

    auto leader_opt = _leaders.local().get_leader(model::tx_manager_ntp);

    auto retries = _metadata_dissemination_retries;
    auto delay_ms = _metadata_dissemination_retry_delay_ms;
    auto aborted = false;
    while (!aborted && !leader_opt && 0 < retries--) {
        aborted = !co_await sleep_abortable(delay_ms);
        leader_opt = _leaders.local().get_leader(model::tx_manager_ntp);
    }

    if (!leader_opt) {
        vlog(
          clusterlog.warn, "can't find a leader for {}", model::tx_manager_ntp);
        co_return cluster::init_tm_tx_reply{.ec = tx_errc::leader_not_found};
    }

    auto leader = leader_opt.value();
    auto _self = _controller->self();

    if (leader == _self) {
        co_return co_await init_tm_tx_locally(
          tx_id, transaction_timeout_ms, timeout);
    }

    vlog(
      clusterlog.trace, "dispatching init_tm_tx to {} from {}", leader, _self);

    co_return co_await dispatch_init_tm_tx(
      leader, tx_id, transaction_timeout_ms, timeout);
}

ss::future<cluster::init_tm_tx_reply> tx_gateway_frontend::init_tm_tx_locally(
  kafka::transactional_id tx_id,
  std::chrono::milliseconds transaction_timeout_ms,
  model::timeout_clock::duration timeout) {
    auto shard = _shard_table.local().shard_for(model::tx_manager_ntp);

    auto retries = _metadata_dissemination_retries;
    auto delay_ms = _metadata_dissemination_retry_delay_ms;
    auto aborted = false;
    while (!aborted && !shard && 0 < retries--) {
        aborted = !co_await sleep_abortable(delay_ms);
        shard = _shard_table.local().shard_for(model::tx_manager_ntp);
    }

    if (!shard) {
        vlog(
          clusterlog.warn, "can't find a shard for {}", model::tx_manager_ntp);
        co_return cluster::init_tm_tx_reply{.ec = tx_errc::shard_not_found};
    }

    co_return co_await do_init_tm_tx(
      *shard, tx_id, transaction_timeout_ms, timeout);
}

ss::future<init_tm_tx_reply> tx_gateway_frontend::dispatch_init_tm_tx(
  model::node_id leader,
  kafka::transactional_id tx_id,
  std::chrono::milliseconds transaction_timeout_ms,
  model::timeout_clock::duration timeout) {
    return _connection_cache.local()
      .with_node_client<cluster::tx_gateway_client_protocol>(
        _controller->self(),
        ss::this_shard_id(),
        leader,
        timeout,
        [tx_id, transaction_timeout_ms, timeout](
          tx_gateway_client_protocol cp) {
            return cp.init_tm_tx(
              init_tm_tx_request{
                .tx_id = tx_id,
                .transaction_timeout_ms = transaction_timeout_ms,
                .timeout = timeout},
              rpc::client_opts(model::timeout_clock::now() + timeout));
        })
      .then(&rpc::get_ctx_data<init_tm_tx_reply>)
      .then([](result<init_tm_tx_reply> r) {
          if (r.has_error()) {
              vlog(
                clusterlog.warn,
                "got error {} on remote init tm tx",
                r.error());
              return init_tm_tx_reply{.ec = tx_errc::unknown_server_error};
          }

          return r.value();
      });
}

ss::future<init_tm_tx_reply> tx_gateway_frontend::do_init_tm_tx(
  ss::shard_id shard,
  kafka::transactional_id tx_id,
  std::chrono::milliseconds transaction_timeout_ms,
  model::timeout_clock::duration timeout) {
    return container().invoke_on(
      shard,
      _ssg,
      [tx_id, transaction_timeout_ms, timeout](tx_gateway_frontend& self) {
          auto partition = self._partition_manager.local().get(
            model::tx_manager_ntp);
          if (!partition) {
              vlog(
                clusterlog.warn,
                "can't get partition by {} ntp",
                model::tx_manager_ntp);
              return ss::make_ready_future<init_tm_tx_reply>(
                init_tm_tx_reply{.ec = tx_errc::partition_not_found});
          }

          auto stm = partition->tm_stm();

          if (!stm) {
              vlog(
                clusterlog.warn,
                "can't get tm stm of the {}' partition",
                model::tx_manager_ntp);
              return ss::make_ready_future<init_tm_tx_reply>(
                init_tm_tx_reply{.ec = tx_errc::stm_not_found});
          }

          return stm->get_tx_lock(tx_id)->with(
            [&self, stm, tx_id, transaction_timeout_ms, timeout]() {
                return self.do_init_tm_tx(
                  stm, tx_id, transaction_timeout_ms, timeout);
            });
      });
}

ss::future<cluster::init_tm_tx_reply> tx_gateway_frontend::do_init_tm_tx(
  ss::shared_ptr<tm_stm> stm,
  kafka::transactional_id tx_id,
  std::chrono::milliseconds transaction_timeout_ms,
  model::timeout_clock::duration timeout) {
    auto maybe_tx = co_await stm->get_actual_tx(tx_id);

    if (!maybe_tx) {
        allocate_id_reply pid_reply
          = co_await _id_allocator_frontend.local().allocate_id(timeout);
        if (pid_reply.ec != errc::success) {
            vlog(clusterlog.warn, "allocate_id failed with {}", pid_reply.ec);
            co_return init_tm_tx_reply{.ec = tx_errc::unknown_server_error};
        }

        model::producer_identity pid{.id = pid_reply.id, .epoch = 0};
        tm_stm::op_status op_status = co_await stm->register_new_producer(
          tx_id, transaction_timeout_ms, pid);
        init_tm_tx_reply reply{.pid = pid};
        if (op_status == tm_stm::op_status::success) {
            reply.ec = tx_errc::none;
        } else if (op_status == tm_stm::op_status::conflict) {
            vlog(
              clusterlog.warn,
              "can't register new producer status: {}",
              op_status);
            reply.ec = tx_errc::conflict;
        } else {
            vlog(
              clusterlog.warn,
              "can't register new producer status: {}",
              op_status);
            reply.ec = tx_errc::unknown_server_error;
        }
        co_return reply;
    }

    auto tx = maybe_tx.value();

    checked<tm_transaction, tx_errc> r(tx);

    if (tx.status == tm_transaction::tx_status::ready) {
        // already in a good state, we don't need to do nothing. even if
        // tx's etag is old it will be bumped by re_register_producer
    } else if (tx.status == tm_transaction::tx_status::ongoing) {
        r = co_await do_abort_tm_tx(
          stm, tx, timeout, ss::make_lw_shared<available_promise<tx_errc>>());
    } else if (tx.status == tm_transaction::tx_status::preparing) {
        r = co_await do_commit_tm_tx(
          stm, tx, timeout, ss::make_lw_shared<available_promise<tx_errc>>());
    } else {
        tx_errc ec;
        if (tx.status == tm_transaction::tx_status::prepared) {
            ec = co_await recommit_tm_tx(tx, timeout);
        } else if (tx.status == tm_transaction::tx_status::aborting) {
            ec = co_await reabort_tm_tx(tx, timeout);
        } else if (tx.status == tm_transaction::tx_status::killed) {
            ec = co_await reabort_tm_tx(tx, timeout);
        } else {
            vassert(false, "unexpected tx status {}", tx.status);
        }

        if (ec != tx_errc::none) {
            r = ec;
        }
    }

    if (!r.has_value()) {
        co_return init_tm_tx_reply{.ec = tx_errc::unknown_server_error};
    }

    tx = r.value();
    init_tm_tx_reply reply;
    if (tx.pid.epoch < std::numeric_limits<int16_t>::max()) {
        reply.pid = model::producer_identity{
          .id = tx.pid.id, .epoch = static_cast<int16_t>(tx.pid.epoch + 1)};
    } else {
        allocate_id_reply pid_reply
          = co_await _id_allocator_frontend.local().allocate_id(timeout);
        reply.pid = model::producer_identity{.id = pid_reply.id, .epoch = 0};
    }

    auto op_status = co_await stm->re_register_producer(
      tx.id, transaction_timeout_ms, reply.pid);
    if (op_status == tm_stm::op_status::success) {
        reply.ec = tx_errc::none;
    } else if (op_status == tm_stm::op_status::conflict) {
        reply.ec = tx_errc::conflict;
    } else {
        reply.ec = tx_errc::unknown_server_error;
    }
    co_return reply;
}

ss::future<add_paritions_tx_reply> tx_gateway_frontend::add_partition_to_tx(
  add_paritions_tx_request request, model::timeout_clock::duration timeout) {
    auto shard = _shard_table.local().shard_for(model::tx_manager_ntp);

    if (shard == std::nullopt) {
        vlog(
          clusterlog.warn, "can't find a shard for {}", model::tx_manager_ntp);
        return ss::make_ready_future<add_paritions_tx_reply>(
          make_add_partitions_error_response(
            request, tx_errc::unknown_server_error));
    }

    return container().invoke_on(
      *shard, _ssg, [request, timeout](tx_gateway_frontend& self) {
          auto partition = self._partition_manager.local().get(
            model::tx_manager_ntp);
          if (!partition) {
              vlog(
                clusterlog.warn,
                "can't get partition by {} ntp",
                model::tx_manager_ntp);
              return ss::make_ready_future<add_paritions_tx_reply>(
                make_add_partitions_error_response(
                  request, tx_errc::unknown_server_error));
          }

          auto stm = partition->tm_stm();

          if (!stm) {
              vlog(
                clusterlog.warn,
                "can't get tm stm of the {}' partition",
                model::tx_manager_ntp);
              return ss::make_ready_future<add_paritions_tx_reply>(
                make_add_partitions_error_response(
                  request, tx_errc::unknown_server_error));
          }

          return stm->get_tx_lock(request.transactional_id)
            ->with([&self, stm, request, timeout]() {
                return self.do_add_partition_to_tx(stm, request, timeout);
            });
      });
}

ss::future<add_paritions_tx_reply> tx_gateway_frontend::do_add_partition_to_tx(
  ss::shared_ptr<tm_stm> stm,
  add_paritions_tx_request request,
  model::timeout_clock::duration timeout) {
    model::producer_identity pid{
      .id = request.producer_id, .epoch = request.producer_epoch};

    auto r = co_await get_ongoing_tx(
      stm, pid, request.transactional_id, timeout);

    if (!r.has_value()) {
        co_return make_add_partitions_error_response(
          request, tx_errc::unknown_server_error);
    }

    auto tx = r.value();
    co_return co_await do_add_partition_to_tx(
      std::move(tx), stm, request, timeout);
}

ss::future<add_paritions_tx_reply> tx_gateway_frontend::do_add_partition_to_tx(
  tm_transaction tx,
  ss::shared_ptr<tm_stm> stm,
  add_paritions_tx_request request,
  model::timeout_clock::duration timeout) {
    add_paritions_tx_reply response;

    std::vector<ss::future<begin_tx_reply>> bfs;

    for (auto& req_topic : request.topics) {
        add_paritions_tx_reply::topic_result res_topic;
        res_topic.name = req_topic.name;

        model::topic topic(req_topic.name);

        res_topic.results.reserve(req_topic.partitions.size());
        for (model::partition_id req_partition : req_topic.partitions) {
            model::ntp ntp(model::kafka_namespace, topic, req_partition);
            auto has_ntp = std::any_of(
              tx.partitions.begin(),
              tx.partitions.end(),
              [ntp](const auto& rm) { return rm.ntp == ntp; });
            if (has_ntp) {
                add_paritions_tx_reply::partition_result res_partition;
                res_partition.partition_index = req_partition;
                res_partition.error_code = tx_errc::none;
                res_topic.results.push_back(res_partition);
            } else {
                bfs.push_back(_rm_partition_frontend.local().begin_tx(
                  ntp, tx.pid, tx.tx_seq, tx.timeout_ms, timeout));
            }
        }
        response.results.push_back(res_topic);
    }

    return when_all_succeed(bfs.begin(), bfs.end())
      .then([response, tx, stm](std::vector<begin_tx_reply> brs) mutable {
          std::vector<tm_transaction::tx_partition> partitions;
          for (auto& br : brs) {
              auto topic_it = std::find_if(
                response.results.begin(),
                response.results.end(),
                [&br](const auto& r) { return r.name == br.ntp.tp.topic(); });
              vassert(
                topic_it != response.results.end(),
                "can't find expected topic {}",
                br.ntp.tp.topic());
              vassert(
                std::none_of(
                  topic_it->results.begin(),
                  topic_it->results.end(),
                  [&br](const auto& r) {
                      return r.partition_index == br.ntp.tp.partition();
                  }),
                "partition {} is already part of the response",
                br.ntp.tp.partition());
              if (br.ec == tx_errc::none) {
                  partitions.push_back(tm_transaction::tx_partition{
                    .ntp = br.ntp, .etag = br.etag});
              }
          }
          auto has_added = stm->add_partitions(tx.id, partitions);
          for (auto& br : brs) {
              auto topic_it = std::find_if(
                response.results.begin(),
                response.results.end(),
                [&br](const auto& r) { return r.name == br.ntp.tp.topic(); });

              add_paritions_tx_reply::partition_result res_partition;
              res_partition.partition_index = br.ntp.tp.partition;
              if (has_added && br.ec == tx_errc::none) {
                  res_partition.error_code = tx_errc::none;
              } else {
                  res_partition.error_code = tx_errc::unknown_server_error;
              }
              topic_it->results.push_back(res_partition);
          }
          return response;
      });
}

ss::future<add_offsets_tx_reply> tx_gateway_frontend::add_offsets_to_tx(
  add_offsets_tx_request request, model::timeout_clock::duration timeout) {
    auto shard = _shard_table.local().shard_for(model::tx_manager_ntp);

    if (shard == std::nullopt) {
        vlog(
          clusterlog.warn, "can't find a shard for {}", model::tx_manager_ntp);
        return ss::make_ready_future<add_offsets_tx_reply>(
          add_offsets_tx_reply{.error_code = tx_errc::unknown_server_error});
    }

    return container().invoke_on(
      *shard, _ssg, [request, timeout](tx_gateway_frontend& self) {
          auto partition = self._partition_manager.local().get(
            model::tx_manager_ntp);
          if (!partition) {
              vlog(
                clusterlog.warn,
                "can't get partition by {} ntp",
                model::tx_manager_ntp);
              return ss::make_ready_future<add_offsets_tx_reply>(
                add_offsets_tx_reply{
                  .error_code = tx_errc::unknown_server_error});
          }

          auto stm = partition->tm_stm();

          if (!stm) {
              vlog(
                clusterlog.warn,
                "can't get tm stm of the {}' partition",
                model::tx_manager_ntp);
              return ss::make_ready_future<add_offsets_tx_reply>(
                add_offsets_tx_reply{
                  .error_code = tx_errc::unknown_server_error});
          }

          return stm->get_tx_lock(request.transactional_id)
            ->with([&self, stm, request, timeout]() {
                return self.do_add_offsets_to_tx(stm, request, timeout);
            });
      });
}

ss::future<add_offsets_tx_reply> tx_gateway_frontend::do_add_offsets_to_tx(
  ss::shared_ptr<tm_stm> stm,
  add_offsets_tx_request request,
  model::timeout_clock::duration timeout) {
    model::producer_identity pid{
      .id = request.producer_id, .epoch = request.producer_epoch};

    auto r = co_await get_ongoing_tx(
      stm, pid, request.transactional_id, timeout);
    if (!r.has_value()) {
        co_return add_offsets_tx_reply{
          .error_code = tx_errc::unknown_server_error};
    }
    auto tx = r.value();

    auto group_info = co_await _rm_group_proxy->begin_group_tx(
      request.group_id, pid, tx.tx_seq, timeout);
    if (group_info.ec != tx_errc::none) {
        vlog(clusterlog.warn, "error on begining group tx: {}", group_info.ec);
        co_return add_offsets_tx_reply{.error_code = group_info.ec};
    }

    auto has_added = stm->add_group(tx.id, request.group_id, group_info.etag);
    if (!has_added) {
        vlog(clusterlog.warn, "can't add group to tm_stm");
        co_return add_offsets_tx_reply{
          .error_code = tx_errc::unknown_server_error};
    }
    co_return add_offsets_tx_reply{.error_code = tx_errc::none};
}

ss::future<end_tx_reply> tx_gateway_frontend::end_txn(
  end_tx_request request, model::timeout_clock::duration timeout) {
    auto shard = _shard_table.local().shard_for(model::tx_manager_ntp);

    if (shard == std::nullopt) {
        vlog(
          clusterlog.warn, "can't find a shard for {}", model::tx_manager_ntp);
        return ss::make_ready_future<end_tx_reply>(
          end_tx_reply{.error_code = tx_errc::unknown_server_error});
    }

    return container().invoke_on(
      *shard,
      _ssg,
      [request = std::move(request), timeout](tx_gateway_frontend& self) {
          auto partition = self._partition_manager.local().get(
            model::tx_manager_ntp);
          if (!partition) {
              vlog(
                clusterlog.warn,
                "can't get partition by {} ntp",
                model::tx_manager_ntp);
              return ss::make_ready_future<end_tx_reply>(
                end_tx_reply{.error_code = tx_errc::unknown_server_error});
          }

          auto stm = partition->tm_stm();

          if (!stm) {
              vlog(
                clusterlog.warn,
                "can't get tm stm of the {}' partition",
                model::tx_manager_ntp);
              return ss::make_ready_future<end_tx_reply>(
                end_tx_reply{.error_code = tx_errc::unknown_server_error});
          }

          auto outcome = ss::make_lw_shared<available_promise<tx_errc>>();
          // commit_tm_tx and abort_tm_tx remove transient data during its
          // execution. however the outcome of the commit/abort operation
          // is already known before the cleanup started. to optimize this
          // they return the outcome promise to return the outcome before
          // cleaning up and before returing the actual control flow
          auto decided = outcome->get_future();

          (void)ss::with_gate(
            self._gate,
            [request = std::move(request),
             &self,
             stm,
             timeout,
             outcome]() mutable {
                return stm->get_tx_lock(request.transactional_id)
                  ->with([request = std::move(request),
                          &self,
                          stm,
                          timeout,
                          outcome]() mutable {
                      return self
                        .do_end_txn(std::move(request), stm, timeout, outcome)
                        .finally([outcome]() {
                            if (!outcome->available()) {
                                outcome->set_value(
                                  tx_errc::unknown_server_error);
                            }
                        });
                  });
            });

          return decided.then(
            [](tx_errc ec) { return end_tx_reply{.error_code = ec}; });
      });
}

ss::future<checked<cluster::tm_transaction, tx_errc>>
tx_gateway_frontend::do_end_txn(
  end_tx_request request,
  ss::shared_ptr<cluster::tm_stm> stm,
  model::timeout_clock::duration timeout,
  ss::lw_shared_ptr<available_promise<tx_errc>> outcome) {
    auto maybe_tx = co_await stm->get_actual_tx(request.transactional_id);
    if (!maybe_tx) {
        outcome->set_value(tx_errc::request_rejected);
        co_return tx_errc::request_rejected;
    }

    model::producer_identity pid{
      .id = request.producer_id, .epoch = request.producer_epoch};
    auto tx = maybe_tx.value();
    if (tx.pid != pid) {
        if (tx.pid.id == pid.id && tx.pid.epoch > pid.epoch) {
            outcome->set_value(tx_errc::fenced);
            co_return tx_errc::fenced;
        }

        outcome->set_value(tx_errc::request_rejected);
        co_return tx_errc::request_rejected;
    }

    checked<cluster::tm_transaction, tx_errc> r(tx_errc::unknown_server_error);
    if (request.committed) {
        if (tx.status == tm_transaction::tx_status::ongoing) {
            r = co_await do_commit_tm_tx(stm, tx, timeout, outcome);
        } else {
            outcome->set_value(tx_errc::request_rejected);
            r = tx_errc::request_rejected;
        }
    } else {
        r = co_await do_abort_tm_tx(stm, tx, timeout, outcome);
    }
    if (!r.has_value()) {
        co_return r;
    }
    tx = r.value();

    auto ongoing_tx = stm->mark_tx_ongoing(tx.id);
    if (!ongoing_tx.has_value()) {
        co_return tx_errc::unknown_server_error;
    }
    co_return ongoing_tx.value();
}

ss::future<checked<cluster::tm_transaction, tx_errc>>
tx_gateway_frontend::do_abort_tm_tx(
  ss::shared_ptr<cluster::tm_stm> stm,
  cluster::tm_transaction tx,
  model::timeout_clock::duration timeout,
  ss::lw_shared_ptr<available_promise<tx_errc>> outcome) {
    if (tx.status == tm_transaction::tx_status::ready) {
        if (stm->is_actual_term(tx.etag)) {
            // client should start a transaction before attempting to
            // abort it. since tx has actual term we know for sure it
            // wasn't start on a different leader
            outcome->set_value(tx_errc::request_rejected);
            co_return tx_errc::request_rejected;
        }

        // writing ready status to overwrite an ongoing transaction if
        // it exists on an older leader
        auto ready_tx = co_await stm->mark_tx_ready(tx.id);
        if (!ready_tx.has_value()) {
            outcome->set_value(tx_errc::unknown_server_error);
            co_return tx_errc::unknown_server_error;
        }
        outcome->set_value(tx_errc::none);
        co_return ready_tx.value();
    } else if (
      tx.status != tm_transaction::tx_status::ongoing
      && tx.status != tm_transaction::tx_status::killed) {
        outcome->set_value(tx_errc::unknown_server_error);
        co_return tx_errc::unknown_server_error;
    }

    if (tx.status == tm_transaction::tx_status::ongoing) {
        auto changed_tx = co_await stm->try_change_status(
          tx.id, cluster::tm_transaction::tx_status::aborting);
        if (!changed_tx.has_value()) {
            outcome->set_value(tx_errc::unknown_server_error);
            co_return tx_errc::unknown_server_error;
        }
        tx = changed_tx.value();
    }
    outcome->set_value(tx_errc::none);

    std::vector<ss::future<abort_tx_reply>> pfs;
    for (auto rm : tx.partitions) {
        pfs.push_back(_rm_partition_frontend.local().abort_tx(
          rm.ntp, tx.pid, tx.tx_seq, timeout));
    }
    std::vector<ss::future<abort_group_tx_reply>> gfs;
    for (auto group : tx.groups) {
        gfs.push_back(
          _rm_group_proxy->abort_group_tx(group.group_id, tx.pid, timeout));
    }
    auto prs = co_await when_all_succeed(pfs.begin(), pfs.end());
    auto grs = co_await when_all_succeed(gfs.begin(), gfs.end());
    bool ok = true;
    for (const auto& r : prs) {
        ok = ok && (r.ec == tx_errc::none);
    }
    for (const auto& r : grs) {
        ok = ok && (r.ec == tx_errc::none);
    }
    if (!ok) {
        co_return tx_errc::unknown_server_error;
    }
    co_return tx;
}

ss::future<checked<cluster::tm_transaction, tx_errc>>
tx_gateway_frontend::do_commit_tm_tx(
  ss::shared_ptr<cluster::tm_stm> stm,
  cluster::tm_transaction tx,
  model::timeout_clock::duration timeout,
  ss::lw_shared_ptr<available_promise<tx_errc>> outcome) {
    if (
      tx.status != tm_transaction::tx_status::ongoing
      && tx.status != tm_transaction::tx_status::preparing) {
        outcome->set_value(tx_errc::request_rejected);
        co_return tx_errc::request_rejected;
    }

    std::vector<ss::future<prepare_tx_reply>> pfs;
    for (auto rm : tx.partitions) {
        pfs.push_back(_rm_partition_frontend.local().prepare_tx(
          rm.ntp,
          rm.etag,
          model::tx_manager_ntp.tp.partition,
          tx.pid,
          tx.tx_seq,
          timeout));
    }

    std::vector<ss::future<prepare_group_tx_reply>> pgfs;
    for (auto group : tx.groups) {
        pgfs.push_back(_rm_group_proxy->prepare_group_tx(
          group.group_id, group.etag, tx.pid, tx.tx_seq, timeout));
    }

    if (tx.status == tm_transaction::tx_status::ongoing) {
        auto became_preparing_tx = co_await stm->try_change_status(
          tx.id, cluster::tm_transaction::tx_status::preparing);
        if (!became_preparing_tx.has_value()) {
            outcome->set_value(tx_errc::unknown_server_error);
            co_return tx_errc::unknown_server_error;
        }
        tx = became_preparing_tx.value();
    }

    auto ok = true;
    auto rejected = false;
    auto prs = co_await when_all_succeed(pfs.begin(), pfs.end());
    for (const auto& r : prs) {
        ok = ok && (r.ec == tx_errc::none);
        rejected = rejected || (r.ec == tx_errc::request_rejected);
    }
    auto pgrs = co_await when_all_succeed(pgfs.begin(), pgfs.end());
    for (const auto& r : pgrs) {
        ok = ok && (r.ec == tx_errc::none);
        rejected = rejected || (r.ec == tx_errc::request_rejected);
    }
    if (rejected) {
        auto became_aborting_tx = co_await stm->try_change_status(
          tx.id, cluster::tm_transaction::tx_status::killed);
        if (!became_aborting_tx.has_value()) {
            outcome->set_value(tx_errc::unknown_server_error);
            co_return tx_errc::unknown_server_error;
        }
        outcome->set_value(tx_errc::request_rejected);
        co_return tx_errc::request_rejected;
    }
    if (!ok) {
        outcome->set_value(tx_errc::unknown_server_error);
        co_return tx_errc::unknown_server_error;
    }
    outcome->set_value(tx_errc::none);

    auto changed_tx = co_await stm->try_change_status(
      tx.id, cluster::tm_transaction::tx_status::prepared);
    if (!changed_tx.has_value()) {
        co_return tx_errc::unknown_server_error;
    }
    tx = changed_tx.value();

    std::vector<ss::future<commit_group_tx_reply>> gfs;
    for (auto group : tx.groups) {
        gfs.push_back(_rm_group_proxy->commit_group_tx(
          group.group_id, tx.pid, tx.tx_seq, timeout));
    }
    std::vector<ss::future<commit_tx_reply>> cfs;
    for (auto rm : tx.partitions) {
        cfs.push_back(_rm_partition_frontend.local().commit_tx(
          rm.ntp, tx.pid, tx.tx_seq, timeout));
    }
    ok = true;
    auto grs = co_await when_all_succeed(gfs.begin(), gfs.end());
    for (const auto& r : grs) {
        ok = ok && (r.ec == tx_errc::none);
    }
    auto crs = co_await when_all_succeed(cfs.begin(), cfs.end());
    for (const auto& r : crs) {
        ok = ok && (r.ec == tx_errc::none);
    }
    if (!ok) {
        co_return tx_errc::unknown_server_error;
    }
    co_return tx;
}

ss::future<tx_errc> tx_gateway_frontend::recommit_tm_tx(
  tm_transaction tx, model::timeout_clock::duration timeout) {
    std::vector<ss::future<commit_group_tx_reply>> gfs;
    for (auto group : tx.groups) {
        gfs.push_back(_rm_group_proxy->commit_group_tx(
          group.group_id, tx.pid, tx.tx_seq, timeout));
    }
    std::vector<ss::future<commit_tx_reply>> cfs;
    for (auto rm : tx.partitions) {
        cfs.push_back(_rm_partition_frontend.local().commit_tx(
          rm.ntp, tx.pid, tx.tx_seq, timeout));
    }

    auto ok = true;
    auto grs = co_await when_all_succeed(gfs.begin(), gfs.end());
    for (const auto& r : grs) {
        ok = ok && (r.ec == tx_errc::none);
    }
    auto crs = co_await when_all_succeed(cfs.begin(), cfs.end());
    for (const auto& r : crs) {
        ok = ok && (r.ec == tx_errc::none);
    }
    if (!ok) {
        co_return tx_errc::unknown_server_error;
    }
    co_return tx_errc::none;
}

ss::future<tx_errc> tx_gateway_frontend::reabort_tm_tx(
  tm_transaction tx, model::timeout_clock::duration timeout) {
    std::vector<ss::future<abort_tx_reply>> pfs;
    for (auto rm : tx.partitions) {
        pfs.push_back(_rm_partition_frontend.local().abort_tx(
          rm.ntp, tx.pid, tx.tx_seq, timeout));
    }
    std::vector<ss::future<abort_group_tx_reply>> gfs;
    for (auto group : tx.groups) {
        gfs.push_back(
          _rm_group_proxy->abort_group_tx(group.group_id, tx.pid, timeout));
    }
    auto prs = co_await when_all_succeed(pfs.begin(), pfs.end());
    auto grs = co_await when_all_succeed(gfs.begin(), gfs.end());
    auto ok = true;
    for (const auto& r : prs) {
        ok = ok && (r.ec == tx_errc::none);
    }
    for (const auto& r : grs) {
        ok = ok && (r.ec == tx_errc::none);
    }
    if (!ok) {
        co_return tx_errc::unknown_server_error;
    }
    co_return tx_errc::none;
}

// get_tx must be called under stm->get_tx_lock
ss::future<checked<tm_transaction, tx_errc>>
tx_gateway_frontend::get_ongoing_tx(
  ss::shared_ptr<tm_stm> stm,
  model::producer_identity pid,
  kafka::transactional_id transactional_id,
  model::timeout_clock::duration timeout) {
    auto maybe_tx = co_await stm->get_actual_tx(transactional_id);
    if (!maybe_tx) {
        co_return tx_errc::request_rejected;
    }

    auto tx = maybe_tx.value();

    if (tx.pid != pid) {
        co_return tx_errc::request_rejected;
    }

    if (tx.status == tm_transaction::tx_status::ready) {
        if (!stm->is_actual_term(tx.etag)) {
            // There is a possibility that a transaction was already started on
            // a previous leader. Failing this request since it has a chance of
            // being a part of that transaction. We expect client to abort on
            // error and the abort will bump the tx's term (etag)
            co_return tx_errc::request_rejected;
        }
    } else if (tx.status == tm_transaction::tx_status::ongoing) {
        if (!stm->is_actual_term(tx.etag)) {
            // Intentnally empty body. Leaving it just to comment what's going
            // on.
            //
            // We don't save ongoing state to the log so the only case when it's
            // possible to observe old ongoing transaction is when the tx was
            // started on this node. Then an another node became leader, a
            // client didn't issue any new requests to that node, the current
            // node became leader again and the client woke up. It's safe to
            // continue it.
        }
        co_return tx;
    } else if (tx.status == tm_transaction::tx_status::preparing) {
        // a producer can see a transaction with the same pid and in a
        // preparing state only if it attempted a commit, the commit
        // failed and then the producer ignored it and tried to start
        // another transaction.
        //
        // it violates the docs, the producer is expected to call abort
        // https://kafka.apache.org/23/javadoc/org/apache/kafka/clients/producer/KafkaProducer.html
        co_return tx_errc::request_rejected;
    } else if (tx.status == tm_transaction::tx_status::killed) {
        // a tx was timed out, can't treat it as ::aborting because
        // from the client perspective it will look like a tx wasn't
        // failed at all but in fact the second part of the tx will
        // start a new transactions
        co_return tx_errc::request_rejected;
    } else {
        // A previous transaction has failed after its status has been
        // decided, rolling it forward.
        tx_errc ec;
        if (tx.status == tm_transaction::tx_status::prepared) {
            ec = co_await recommit_tm_tx(tx, timeout);
        } else if (tx.status == tm_transaction::tx_status::aborting) {
            ec = co_await reabort_tm_tx(tx, timeout);
        } else {
            vassert(false, "unexpected tx status {}", tx.status);
        }

        if (ec != tx_errc::none) {
            co_return ec;
        }

        if (!stm->is_actual_term(tx.etag)) {
            // The tx has started on the previous term. Even though we rolled it
            // forward there is a possibility that a previous leader did the
            // same and already started the current transaction.
            //
            // Failing the current request. By the spec a client should abort on
            // failure, but abort doesn't handle prepared and aborting statuses.
            // So marking it as ready. We use previous term because aborting a
            // tx in current ready state with current term means we abort a tx
            // which wasn't started and it leads to an error.
            (void)co_await stm->mark_tx_ready(tx.id, tx.etag);
            co_return tx_errc::unknown_server_error;
        }
    }

    auto ongoing_tx = stm->mark_tx_ongoing(tx.id);
    if (!ongoing_tx.has_value()) {
        co_return tx_errc::unknown_server_error;
    }
    co_return ongoing_tx.value();
}

ss::future<bool> tx_gateway_frontend::try_create_tx_topic() {
    cluster::topic_configuration topic{
      model::kafka_internal_namespace,
      model::tx_manager_topic,
      1,
      config::shard_local_cfg().transaction_coordinator_replication()};

    topic.properties.cleanup_policy_bitflags
      = config::shard_local_cfg().transaction_coordinator_cleanup_policy();

    return _controller->get_topics_frontend()
      .local()
      .autocreate_topics(
        {std::move(topic)}, config::shard_local_cfg().create_topic_timeout_ms())
      .then([](std::vector<cluster::topic_result> res) {
          vassert(res.size() == 1, "expected exactly one result");
          if (res[0].ec != cluster::errc::success) {
              return false;
          }
          return true;
      })
      .handle_exception([](std::exception_ptr e) {
          vlog(clusterlog.warn, "cant create tx manager topic {}", e);
          return false;
      });
}

} // namespace cluster
