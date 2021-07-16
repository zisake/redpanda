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

#include "bytes/iobuf.h"
#include "json/json.h"
#include "kafka/protocol/errors.h"
#include "kafka/types.h"
#include "pandaproxy/json/iobuf.h"
#include "pandaproxy/json/rjson_parse.h"
#include "seastarx.h"
#include "utils/string_switch.h"

#include <seastar/core/sstring.hh>

#include <rapidjson/reader.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <string_view>

namespace pandaproxy::json {

struct subscribe_consumer_request {
    std::vector<model::topic> topics;
};

template<typename Encoding = rapidjson::UTF8<>>
class subscribe_consumer_request_handler final : public base_handler<Encoding> {
private:
    enum class state {
        empty = 0,
        topics,
        topic_name,
    };

    state _state = state::empty;

public:
    using Ch = typename Encoding::Ch;
    using rjson_parse_result = subscribe_consumer_request;
    rjson_parse_result result;

    bool String(const Ch* str, rapidjson::SizeType len, bool) {
        if (_state != state::topic_name) {
            return false;
        }
        result.topics.emplace_back(ss::sstring(str, len));
        return true;
    }

    bool Key(const char* str, rapidjson::SizeType len, bool) {
        if (_state != state::topics || std::string_view(str, len) != "topics") {
            return false;
        }
        return true;
    }

    bool StartArray() {
        if (_state != state::topics) {
            return false;
        }
        _state = state::topic_name;
        return true;
    }
    bool EndArray(rapidjson::SizeType) {
        if (_state != state::topic_name) {
            return false;
        }
        _state = state::topics;
        return true;
    }

    bool StartObject() {
        if (_state == state::empty) {
            _state = state::topics;
        } else if (_state == state::topics) {
            _state = state::topic_name;
        } else {
            return false;
        }
        return true;
    }

    bool EndObject(rapidjson::SizeType) { return _state == state::topics; }
};

} // namespace pandaproxy::json
