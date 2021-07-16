// Copyright 2021 Vectorized, Inc.
//
// Use of this software is governed by the Business Source License
// included in the file licenses/BSL.md
//
// As of the Change Date specified in that file, in accordance with
// the Business Source License, use of this software will be governed
// by the Apache License, Version 2.0

#include "pandaproxy/schema_registry/configuration.h"

namespace pandaproxy::schema_registry {

configuration::configuration(const YAML::Node& cfg)
  : configuration() {
    read_yaml(cfg);
}

configuration::configuration()
  : schema_registry_api(
    *this,
    "schema_registry_api",
    "Schema Registry API listen address and port",
    config::required::no,
    {model::broker_endpoint(unresolved_address("0.0.0.0", 8081))})
  , schema_registry_api_tls(
      *this,
      "schema_registry_api_tls",
      "TLS configuration for Schema Registry API",
      config::required::no,
      {},
      config::endpoint_tls_config::validate_many)
  , api_doc_dir(
      *this,
      "api_doc_dir",
      "API doc directory",
      config::required::no,
      "/usr/share/redpanda/proxy-api-doc") {}

} // namespace pandaproxy::schema_registry
