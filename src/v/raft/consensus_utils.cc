// Copyright 2020 Vectorized, Inc.
//
// Use of this software is governed by the Business Source License
// included in the file licenses/BSL.md
//
// As of the Change Date specified in that file, in accordance with
// the Business Source License, use of this software will be governed
// by the Apache License, Version 2.0

#include "raft/consensus_utils.h"

#include "likely.h"
#include "model/fundamental.h"
#include "model/record.h"
#include "model/timestamp.h"
#include "raft/group_configuration.h"
#include "raft/logger.h"
#include "raft/types.h"
#include "random/generators.h"
#include "reflection/adl.h"
#include "resource_mgmt/io_priority.h"
#include "storage/api.h"
#include "storage/kvstore.h"
#include "storage/record_batch_builder.h"
#include "vassert.h"

#include <seastar/core/abort_source.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/file-types.hh>
#include <seastar/core/file.hh>
#include <seastar/core/future.hh>
#include <seastar/core/seastar.hh>
#include <seastar/core/thread.hh>

#include <cstring>
#include <filesystem>
// delete
#include <seastar/core/future-util.hh>
#include <seastar/core/when_all.hh>

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <iterator>

namespace raft::details {
[[gnu::cold]] void throw_out_of_range() {
    throw std::out_of_range("consensus_utils copy out of bounds");
}
static inline void check_copy_out_of_range(size_t expected, size_t got) {
    if (unlikely(expected != got)) {
        throw_out_of_range();
    }
}

static inline ss::circular_buffer<model::record_batch>
share_n_record_batch(model::record_batch batch, const size_t copies) {
    using ret_t = ss::circular_buffer<model::record_batch>;
    ret_t ret;
    ret.reserve(copies);
    // the fast path
    std::generate_n(
      std::back_inserter(ret), copies, [batch = std::move(batch)]() mutable {
          return batch.share();
      });
    return ret;
}

static inline ss::future<std::vector<ss::circular_buffer<model::record_batch>>>
share_n_batches(
  ss::circular_buffer<model::record_batch> batches, const size_t copies) {
    using ret_t = std::vector<ss::circular_buffer<model::record_batch>>;
    return do_with(
      std::move(batches),
      ret_t(copies),
      [copies](ss::circular_buffer<model::record_batch>& batches, ret_t& data) {
          return ss::do_for_each(
                   batches,
                   [copies, &data](model::record_batch& b) mutable {
                       auto shared_batches = share_n_record_batch(
                         std::move(b), copies);

                       for (auto& buf : data) {
                           buf.push_back(std::move(shared_batches.back()));
                           shared_batches.pop_back();
                       }
                   })
            .then([&data]() mutable { return std::move(data); });
      });
} // namespace raft::details

ss::future<std::vector<model::record_batch_reader>> share_reader(
  model::record_batch_reader rdr,
  const size_t ncopies,
  const bool use_foreign_share) {
    return model::consume_reader_to_memory(std::move(rdr), model::no_timeout)
      .then([ncopies](ss::circular_buffer<model::record_batch> batches) {
          return share_n_batches(std::move(batches), ncopies);
      })
      .then([ncopies, use_foreign_share](
              std::vector<ss::circular_buffer<model::record_batch>> batches) {
          check_copy_out_of_range(ncopies, batches.size());
          std::vector<model::record_batch_reader> retval;
          retval.reserve(ncopies);
          for (auto& b : batches) {
              auto r = use_foreign_share
                         ? model::make_foreign_memory_record_batch_reader(
                           std::move(b))
                         : model::make_memory_record_batch_reader(std::move(b));
              retval.emplace_back(std::move(r));
          }
          check_copy_out_of_range(ncopies, retval.size());
          return retval;
      });
}

ss::future<std::vector<model::record_batch_reader>>
foreign_share_n(model::record_batch_reader&& r, std::size_t ncopies) {
    return share_reader(std::move(r), ncopies, true);
}

ss::future<std::vector<model::record_batch_reader>>
share_n(model::record_batch_reader&& r, std::size_t ncopies) {
    return share_reader(std::move(r), ncopies, false);
}

ss::future<configuration_bootstrap_state> read_bootstrap_state(
  storage::log log, model::offset start_offset, ss::abort_source& as) {
    // TODO(agallego, michal) - iterate the log in reverse
    // as an optimization
    auto lstats = log.offsets();
    auto rcfg = storage::log_reader_config(
      start_offset, lstats.dirty_offset, raft_priority(), as);
    auto cfg_state = std::make_unique<configuration_bootstrap_state>();
    return log.make_reader(rcfg).then(
      [state = std::move(cfg_state)](
        model::record_batch_reader reader) mutable {
          auto raw = state.get();
          return std::move(reader)
            .consume(
              do_for_each_batch_consumer([raw](model::record_batch batch) {
                  raw->process_batch(std::move(batch));
                  return ss::make_ready_future<>();
              }),
              model::no_timeout)
            .then([s = std::move(state)]() mutable { return std::move(*s); });
      });
}

ss::circular_buffer<model::record_batch>
serialize_configuration_as_batches(group_configuration cfg) {
    auto batch = std::move(
                   storage::record_batch_builder(
                     model::record_batch_type::raft_configuration,
                     model::offset(0))
                     .add_raw_kv(iobuf(), reflection::to_iobuf(std::move(cfg))))
                   .build();
    ss::circular_buffer<model::record_batch> batches;
    batches.reserve(1);
    batches.push_back(std::move(batch));
    return batches;
}

model::record_batch_reader serialize_configuration(group_configuration cfg) {
    return model::make_memory_record_batch_reader(
      serialize_configuration_as_batches(std::move(cfg)));
}

model::record_batch make_ghost_batch(
  model::offset start_offset, model::offset end_offset, model::term_id term) {
    auto delta = end_offset - start_offset;
    auto now = model::timestamp::now();
    model::record_batch_header header = {
      .size_bytes = model::packed_record_batch_header_size,
      .base_offset = start_offset,
      .type = model::record_batch_type::ghost_batch,
      .crc = 0, // crc computed later
      .attrs = model::record_batch_attributes{} |= model::compression::none,
      .last_offset_delta = static_cast<int32_t>(delta),
      .first_timestamp = now,
      .max_timestamp = now,
      .producer_id = -1,
      .producer_epoch = -1,
      .base_sequence = -1,
      .record_count = static_cast<int32_t>(delta() + 1),
      .ctx = model::record_batch_header::context(term, ss::this_shard_id())};

    model::record_batch batch(
      std::move(header), model::record_batch::compressed_records{});

    batch.header().crc = model::crc_record_batch(batch);
    batch.header().header_crc = model::internal_header_only_crc(batch.header());
    return batch;
}

ss::circular_buffer<model::record_batch> make_ghost_batches_in_gaps(
  model::offset expected_start,
  ss::circular_buffer<model::record_batch>&& batches) {
    ss::circular_buffer<model::record_batch> res;
    res.reserve(batches.size());
    for (auto& b : batches) {
        // gap
        if (b.base_offset() > expected_start) {
            res.push_back(make_ghost_batch(
              expected_start, prev_offset(b.base_offset()), b.term()));
        }
        expected_start = next_offset(b.last_offset());
        res.push_back(std::move(b));
    }
    return res;
}

ss::future<> persist_snapshot(
  storage::snapshot_manager& snapshot_manager,
  snapshot_metadata md,
  iobuf&& data) {
    return snapshot_manager.start_snapshot().then(
      [&snapshot_manager, md = std::move(md), data = std::move(data)](
        storage::snapshot_writer writer) mutable {
          return ss::do_with(
            std::move(writer),
            [&snapshot_manager, md = std::move(md), data = std::move(data)](
              storage::snapshot_writer& writer) mutable {
                return writer
                  .write_metadata(reflection::to_iobuf(std::move(md)))
                  .then([&writer, data = std::move(data)]() mutable {
                      return write_iobuf_to_output_stream(
                        std::move(data), writer.output());
                  })
                  .finally([&writer] { return writer.close(); })
                  .then([&snapshot_manager, &writer] {
                      return snapshot_manager.finish_snapshot(writer);
                  });
            });
      });
}

model::record_batch_reader make_config_extracting_reader(
  model::offset base_offset,
  std::vector<offset_configuration>& target,
  model::record_batch_reader&& source) {
    class extracting_reader final : public model::record_batch_reader::impl {
    private:
        using storage_t = model::record_batch_reader::storage_t;
        using data_t = model::record_batch_reader::data_t;
        using foreign_t = model::record_batch_reader::foreign_data_t;

    public:
        explicit extracting_reader(
          model::offset o,
          std::vector<offset_configuration>& target,
          std::unique_ptr<model::record_batch_reader::impl> src)
          : _next_offset(
            o < model::offset(0) ? model::offset(0) : o + model::offset(1))
          , _configurations(target)
          , _ptr(std::move(src)) {}
        extracting_reader(const extracting_reader&) = delete;
        extracting_reader& operator=(const extracting_reader&) = delete;
        extracting_reader(extracting_reader&&) = delete;
        extracting_reader& operator=(extracting_reader&&) = delete;
        ~extracting_reader() override = default;

        bool is_end_of_stream() const final {
            // ok to copy a bool
            return _ptr->is_end_of_stream();
        }

        void print(std::ostream& os) final {
            fmt::print(os, "configuration extracting reader, proxy for ");
            _ptr->print(os);
        }

        data_t& get_batches(storage_t& st) {
            if (std::holds_alternative<data_t>(st)) {
                return std::get<data_t>(st);
            } else {
                return *std::get<foreign_t>(st).buffer;
            }
        }

        ss::future<storage_t>
        do_load_slice(model::timeout_clock::time_point t) final {
            return _ptr->do_load_slice(t).then([this](storage_t recs) {
                for (auto& batch : get_batches(recs)) {
                    if (
                      batch.header().type
                      == model::record_batch_type::raft_configuration) {
                        extract_configuration(batch);
                    }
                    // calculate next offset
                    _next_offset += model::offset(
                                      batch.header().last_offset_delta)
                                    + model::offset(1);
                }
                return recs;
            });
        }

        void extract_configuration(model::record_batch& batch) {
            auto cfg = reflection::from_iobuf<group_configuration>(
              batch.copy_records().begin()->value().copy());
            _configurations.emplace_back(_next_offset, std::move(cfg));
        }

    private:
        model::offset _next_offset;
        std::vector<offset_configuration>& _configurations;
        std::unique_ptr<model::record_batch_reader::impl> _ptr;
    };
    auto reader = std::make_unique<extracting_reader>(
      base_offset, target, std::move(source).release());

    return model::record_batch_reader(std::move(reader));
}

bytes serialize_group_key(raft::group_id group, metadata_key key_type) {
    iobuf buf;
    reflection::serialize(buf, key_type, group);
    return iobuf_to_bytes(buf);
}

ss::future<> move_persistent_state(
  raft::group_id group,
  ss::shard_id source_shard,
  ss::shard_id target_shard,
  ss::sharded<storage::api>& api) {
    struct persistent_state {
        std::optional<iobuf> voted_for;
        std::optional<iobuf> last_applied;
        std::optional<iobuf> unique_run_id;
        std::optional<iobuf> configuration_map;
        std::optional<iobuf> highest_known_offset;
        std::optional<iobuf> next_cfg_idx;
    };
    using state_ptr = std::unique_ptr<persistent_state>;
    using state_fptr = ss::foreign_ptr<std::unique_ptr<persistent_state>>;

    state_fptr state = co_await api.invoke_on(
      source_shard, [gr = group](storage::api& api) {
          const auto ks = storage::kvstore::key_space::consensus;
          persistent_state state{
            .voted_for = api.kvs().get(
              ks, serialize_group_key(gr, metadata_key::voted_for)),
            .last_applied = api.kvs().get(
              ks, serialize_group_key(gr, metadata_key::last_applied_offset)),
            .unique_run_id = api.kvs().get(
              ks, serialize_group_key(gr, metadata_key::unique_local_id)),
            .configuration_map = api.kvs().get(
              ks, serialize_group_key(gr, metadata_key::config_map)),
            .highest_known_offset = api.kvs().get(
              ks,
              serialize_group_key(
                gr, metadata_key::config_latest_known_offset)),
            .next_cfg_idx = api.kvs().get(
              ks, serialize_group_key(gr, metadata_key::config_next_cfg_idx))};
          return ss::make_foreign<state_ptr>(
            std::make_unique<persistent_state>(std::move(state)));
      });

    co_await api.invoke_on(
      target_shard, [gr = group, state = std::move(state)](storage::api& api) {
          const auto ks = storage::kvstore::key_space::consensus;
          std::vector<ss::future<>> write_futures;
          write_futures.reserve(6);
          if (state->voted_for) {
              write_futures.push_back(api.kvs().put(
                ks,
                serialize_group_key(gr, metadata_key::voted_for),
                state->voted_for->copy()));
          }
          if (state->last_applied) {
              write_futures.push_back(api.kvs().put(
                ks,
                serialize_group_key(gr, metadata_key::last_applied_offset),
                state->last_applied->copy()));
          }
          if (state->unique_run_id) {
              write_futures.push_back(api.kvs().put(
                ks,
                serialize_group_key(gr, metadata_key::unique_local_id),
                state->unique_run_id->copy()));
          }
          if (state->configuration_map) {
              write_futures.push_back(api.kvs().put(
                ks,
                serialize_group_key(gr, metadata_key::config_map),
                state->configuration_map->copy()));
          }
          if (state->highest_known_offset) {
              write_futures.push_back(api.kvs().put(
                ks,
                serialize_group_key(
                  gr, metadata_key::config_latest_known_offset),
                state->highest_known_offset->copy()));
          }
          if (state->next_cfg_idx) {
              write_futures.push_back(api.kvs().put(
                ks,
                serialize_group_key(gr, metadata_key::config_next_cfg_idx),
                state->next_cfg_idx->copy()));
          }
          return ss::when_all_succeed(
            write_futures.begin(), write_futures.end());
      });

    // remove on source shard
    co_await api.invoke_on(source_shard, [gr = group](storage::api& api) {
        const auto ks = storage::kvstore::key_space::consensus;
        std::vector<ss::future<>> remove_futures;
        remove_futures.reserve(6);
        remove_futures.push_back(api.kvs().remove(
          ks, serialize_group_key(gr, metadata_key::voted_for)));
        remove_futures.push_back(api.kvs().remove(
          ks, serialize_group_key(gr, metadata_key::last_applied_offset)));
        remove_futures.push_back(api.kvs().remove(
          ks, serialize_group_key(gr, metadata_key::unique_local_id)));
        remove_futures.push_back(api.kvs().remove(
          ks, serialize_group_key(gr, metadata_key::config_map)));
        remove_futures.push_back(api.kvs().remove(
          ks,
          serialize_group_key(gr, metadata_key::config_latest_known_offset)));
        remove_futures.push_back(api.kvs().remove(
          ks, serialize_group_key(gr, metadata_key::config_next_cfg_idx)));
        return ss::when_all_succeed(
          remove_futures.begin(), remove_futures.end());
    });
}

} // namespace raft::details
