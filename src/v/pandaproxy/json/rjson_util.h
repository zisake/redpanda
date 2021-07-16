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

#include "json/json.h"
#include "pandaproxy/json/exceptions.h"
#include "pandaproxy/json/types.h"
#include "utils/concepts-enabled.h"

#include <seastar/core/sstring.hh>

#include <rapidjson/prettywriter.h>
#include <rapidjson/reader.h>
#include <rapidjson/stream.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <stdexcept>

namespace pandaproxy::json {

template<typename T>
ss::sstring rjson_serialize(const T& v) {
    rapidjson::StringBuffer str_buf;
    rapidjson::Writer<rapidjson::StringBuffer> wrt(str_buf);

    using ::json::rjson_serialize;
    using ::pandaproxy::json::rjson_serialize;
    rjson_serialize(wrt, v);

    return ss::sstring(str_buf.GetString(), str_buf.GetSize());
}

struct rjson_serialize_fmt_impl {
    explicit rjson_serialize_fmt_impl(serialization_format fmt)
      : fmt{fmt} {}

    serialization_format fmt;
    template<typename T>
    void operator()(T&& t) {
        rjson_serialize_impl<std::remove_reference_t<T>>{fmt}(
          std::forward<T>(t));
    }
    template<typename T>
    void operator()(rapidjson::Writer<rapidjson::StringBuffer>& w, T&& t) {
        rjson_serialize_impl<std::remove_reference_t<T>>{fmt}(
          w, std::forward<T>(t));
    }
};

inline rjson_serialize_fmt_impl rjson_serialize_fmt(serialization_format fmt) {
    return rjson_serialize_fmt_impl{fmt};
}

template<typename Handler>
CONCEPT(requires std::is_same_v<
        decltype(std::declval<Handler>().result),
        typename Handler::rjson_parse_result>)
typename Handler::rjson_parse_result
  rjson_parse(const char* const s, Handler&& handler) {
    rapidjson::Reader reader;
    rapidjson::StringStream ss(s);
    if (!reader.Parse(ss, handler)) {
        throw parse_error(reader.GetErrorOffset());
    }
    return std::move(handler.result);
}

inline ss::sstring minify(std::string_view json) {
    rapidjson::Reader r;
    rapidjson::StringStream in(json.data());
    rapidjson::StringBuffer out;
    rapidjson::Writer<rapidjson::StringBuffer> w{out};
    r.Parse(in, w);
    return ss::sstring(out.GetString(), out.GetSize());
}

inline ss::sstring prettify(std::string_view json) {
    rapidjson::Reader r;
    rapidjson::StringStream in(json.data());
    rapidjson::StringBuffer out;
    rapidjson::PrettyWriter<rapidjson::StringBuffer> w{out};
    r.Parse(in, w);
    return ss::sstring(out.GetString(), out.GetSize());
}

} // namespace pandaproxy::json
