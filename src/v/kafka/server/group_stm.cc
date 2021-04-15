#include "kafka/server/group_stm.h"

namespace kafka {

void group_stm::overwrite_metadata(group_log_group_metadata&& metadata) {
    _metadata = std::move(metadata);
    _is_loaded = true;
}

void group_stm::remove_offset(model::topic_partition key) {
    _offsets.erase(key);
}

void group_stm::update_offset(
  model::topic_partition key,
  model::offset offset,
  group_log_offset_metadata&& meta) {
    _offsets[key] = logged_metadata{
      .log_offset = offset, .metadata = std::move(meta)};
}

void group_stm::update_prepared(
  model::offset offset, group_log_prepared_tx val) {
    auto tx = group::group_prepared_tx{
      .pid = val.pid, .group_id = val.group_id};

    auto [prepared_it, inserted] = _prepared_txs.try_emplace(
      tx.pid.get_id(), tx);
    if (!inserted && prepared_it->second.pid.epoch > tx.pid.epoch) {
        klog.warn(
          "a logged tx {} is fenced off by prev logged tx {}",
          val.pid,
          prepared_it->second.pid);
        return;
    } else if (!inserted && prepared_it->second.pid.epoch < tx.pid.epoch) {
        klog.warn(
          "a logged tx {} overwrites prev logged tx {}",
          val.pid,
          prepared_it->second.pid);
        prepared_it->second.pid = tx.pid;
        prepared_it->second.offsets.clear();
    }

    for (const auto& tx_offset : val.offsets) {
        group::offset_metadata md{
          .log_offset = offset,
          .offset = tx_offset.offset,
          .metadata = tx_offset.metadata.value_or(""),
        };
        // BUG: support leader_epoch (KIP-320)
        // https://github.com/vectorizedio/redpanda/issues/1181
        prepared_it->second.offsets[tx_offset.tp] = md;
    }
}

std::ostream& operator<<(std::ostream& os, const group_log_offset_key& key) {
    fmt::print(
      os,
      "group {} topic {} partition {}",
      key.group(),
      key.topic(),
      key.partition());
    return os;
}

std::ostream&
operator<<(std::ostream& os, const group_log_offset_metadata& md) {
    fmt::print(os, "offset {}", md.offset());
    return os;
}

} // namespace kafka
