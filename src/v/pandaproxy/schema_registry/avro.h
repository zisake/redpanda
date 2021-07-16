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

#include "pandaproxy/schema_registry/errors.h"
#include "pandaproxy/schema_registry/types.h"

#include <avro/ValidSchema.hh>

namespace pandaproxy::schema_registry {

struct avro_schema_definition
  : named_type<avro::ValidSchema, struct avro_schema_definition_tag> {
    using named_type<avro::ValidSchema, struct avro_schema_definition_tag>::
      named_type;

    explicit operator schema_definition() const {
        return schema_definition{_value.toJson(false)};
    }
};

result<avro_schema_definition> make_avro_schema_definition(std::string_view sv);

bool check_compatible(
  const avro_schema_definition& reader, const avro_schema_definition& writer);

} // namespace pandaproxy::schema_registry
