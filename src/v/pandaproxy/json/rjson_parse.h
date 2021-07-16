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

#include "json/json.h"
#include "pandaproxy/json/types.h"

#include <rapidjson/encodings.h>
#include <rapidjson/reader.h>

namespace pandaproxy::json {

template<typename encoding = rapidjson::UTF8<>>
class base_handler
  : public rapidjson::BaseReaderHandler<encoding, base_handler<encoding>> {
public:
    using Ch = typename encoding::Ch;

    explicit base_handler(serialization_format fmt = serialization_format::none)
      : _fmt{fmt} {}

    bool Default() { return false; }
    serialization_format format() const { return _fmt; }

private:
    serialization_format _fmt{serialization_format::none};
};

} // namespace pandaproxy::json
