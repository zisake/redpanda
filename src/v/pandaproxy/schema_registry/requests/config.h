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

#pragma once

#include "pandaproxy/json/rjson_parse.h"
#include "pandaproxy/json/rjson_util.h"
#include "pandaproxy/schema_registry/types.h"

namespace pandaproxy::schema_registry {

struct get_config_req_rep {
    static constexpr std::string_view field_name = "compatibilityLevel";
    compatibility_level compat{compatibility_level::none};
};

struct put_config_req_rep {
    static constexpr std::string_view field_name = "compatibility";
    compatibility_level compat{compatibility_level::none};
};

template<typename Encoding = rapidjson::UTF8<>>
class put_config_handler : public json::base_handler<Encoding> {
    enum class state {
        empty = 0,
        object,
        compatibility,
    };
    state _state = state::empty;

public:
    using Ch = typename json::base_handler<Encoding>::Ch;
    using rjson_parse_result = put_config_req_rep;
    rjson_parse_result result;

    explicit put_config_handler()
      : json::base_handler<Encoding>{json::serialization_format::none}
      , result() {}

    bool Key(const Ch* str, rapidjson::SizeType len, bool) {
        auto sv = std::string_view{str, len};
        if (_state == state::object && sv == put_config_req_rep::field_name) {
            _state = state::compatibility;
            return true;
        }
        return false;
    }

    bool String(const Ch* str, rapidjson::SizeType len, bool) {
        auto sv = std::string_view{str, len};
        if (_state == state::compatibility) {
            auto s = from_string_view<compatibility_level>(sv);
            if (s.has_value()) {
                result.compat = *s;
                _state = state::object;
            }
            return s.has_value();
        }
        return false;
    }

    bool StartObject() {
        return std::exchange(_state, state::object) == state::empty;
    }

    bool EndObject(rapidjson::SizeType) {
        return std::exchange(_state, state::empty) == state::object;
    }
};

inline void rjson_serialize(
  rapidjson::Writer<rapidjson::StringBuffer>& w,
  const schema_registry::get_config_req_rep& res) {
    w.StartObject();
    w.Key(get_config_req_rep::field_name.data());
    ::json::rjson_serialize(w, to_string_view(res.compat));
    w.EndObject();
}

inline void rjson_serialize(
  rapidjson::Writer<rapidjson::StringBuffer>& w,
  const schema_registry::put_config_req_rep& res) {
    w.StartObject();
    w.Key(put_config_req_rep::field_name.data());
    ::json::rjson_serialize(w, to_string_view(res.compat));
    w.EndObject();
}

} // namespace pandaproxy::schema_registry
