/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright The LanceDB Authors
 *
 * Example: Using the ObjectStoreRegistry APIs
 *
 * This example demonstrates how to:
 * 1. Create a custom ObjectStoreRegistry
 * 2. Create an in-memory ObjectStoreProvider and insert it into the registry
 * 3. Create a session with the custom registry
 * 4. Connect to a memory-backed database
 * 5. Create a table, list tables, and clean up
 *
 * The registry allows customizing which ObjectStoreProvider handles
 * each URL scheme (e.g., "file", "s3", "memory"). By default, a new
 * registry comes with providers for local filesystem, S3, memory, etc.
 * You can override the default provider for a scheme by inserting
 * a custom one via lancedb_registry_insert_provider().
 *
 * The provider is built as a separate Rust crate (examples/s3_provider/)
 * that implements the ObjectStoreProvider trait from lance-io.
 *
 * Usage:
 *   ./lancedb_registry
 */

#include <iostream>
#include <memory>
#include <vector>
#include <arrow/api.h>
#include <arrow/c/bridge.h>
#include "lancedb.h"

// This function is provided by the external_provider crate
// (examples/external_provider/) but is not part of the public lancedb API.
// It creates an external ObjectStoreProvider (initialized to in-memory
// storage for this example) to be inserted into a registry.
extern "C" {
LanceDBObjectStoreProvider* external_object_store_provider_create();
}

constexpr size_t DIM = 128;

auto create_schema() {
  auto id_field = arrow::field("id", arrow::int32());
  auto item_field = arrow::field("item", arrow::fixed_size_list(arrow::float32(), DIM));
  return arrow::schema({id_field, item_field});
}

LanceDBTable* create_empty_table(LanceDBConnection* db) {
  auto schema = create_schema();
  struct ArrowSchema c_schema;
  if (const auto status = arrow::ExportSchema(*schema, &c_schema); !status.ok()) {
    std::cerr << "failed to export schema to C ABI: " << status.ToString() << std::endl;
    return nullptr;
  }

  const std::string table_name = "registry_example";
  LanceDBTable* tbl = nullptr;
  char* error_message = nullptr;
  if (const LanceDBError result = lancedb_table_create(db, table_name.c_str(),
      reinterpret_cast<FFI_ArrowSchema*>(&c_schema),
      nullptr, &tbl, &error_message); result != LANCEDB_SUCCESS) {
    std::cerr << "error creating table: " << table_name
              << ", error: " << error_message << std::endl;
    lancedb_free_string(error_message);
  } else {
    std::cout << "created table: " << table_name << " (empty)" << std::endl;
  }

  if (c_schema.release) {
    c_schema.release(&c_schema);
  }
  return tbl;
}

int main() {
  // Step 1: Create a custom ObjectStoreRegistry (comes with default providers)
  LanceDBObjectStoreRegistry* registry = lancedb_registry_new();
  if (!registry) {
    std::cerr << "failed to create registry" << std::endl;
    return 1;
  }
  std::cout << "created ObjectStoreRegistry with default providers" << std::endl;

  // Step 2: Create an external provider and register it under a custom
  // "custom" scheme. This demonstrates adding a new URL scheme that
  // doesn't exist in the default registry.
  LanceDBObjectStoreProvider* provider = external_object_store_provider_create();
  if (!provider) {
    std::cerr << "failed to create provider" << std::endl;
    lancedb_registry_free(registry);
    return 1;
  }
  std::cout << "created in-memory ObjectStoreProvider" << std::endl;

  char* error_message = nullptr;
  // Ownership of provider transfers to the registry here
  if (const LanceDBError result = lancedb_registry_insert_provider(
      registry, "custom", provider, &error_message); result != LANCEDB_SUCCESS) {
    std::cerr << "failed to insert provider: " << error_message << std::endl;
    lancedb_free_string(error_message);
    lancedb_registry_free(registry);
    return 1;
  }
  std::cout << "inserted provider into registry for scheme \"custom\"" << std::endl;

  // Step 3: Create a session with the custom registry
  // The session takes ownership of the registry (do not free it separately)
  LanceDBSession* session = lancedb_session_new_with_registry(nullptr, registry);
  if (!session) {
    std::cerr << "failed to create session with registry" << std::endl;
    lancedb_registry_free(registry);
    return 1;
  }
  std::cout << "created session with custom registry" << std::endl;

  // Step 4: Connect using the custom "custom://" scheme.
  // This works because we registered a provider for it in step 2.
  const std::string uri = "custom://sample-lancedb-registry";
  LanceDBConnectBuilder* builder = lancedb_connect(uri.c_str());
  if (!builder) {
    std::cerr << "failed to create connection builder" << std::endl;
    lancedb_session_free(session);
    return 1;
  }

  builder = lancedb_connect_builder_session(builder, session);
  if (!builder) {
    std::cerr << "failed to set session on builder" << std::endl;
    lancedb_session_free(session);
    return 1;
  }

  LanceDBConnection* db = lancedb_connect_builder_execute(builder);
  if (!db) {
    std::cerr << "failed to connect to database" << std::endl;
    lancedb_session_free(session);
    return 1;
  }
  std::cout << "connected to: " << lancedb_connection_uri(db) << std::endl;

  // Step 5: Create a table
  auto tbl = create_empty_table(db);
  if (!tbl) {
    lancedb_connection_free(db);
    lancedb_session_free(session);
    return 1;
  }
  lancedb_table_free(tbl);

  // Step 6: List tables
  char** table_names = nullptr;
  size_t name_count = 0;
  if (const LanceDBError result = lancedb_connection_table_names(
      db, &table_names, &name_count, &error_message); result != LANCEDB_SUCCESS) {
    std::cerr << "error listing tables: " << error_message << std::endl;
    lancedb_free_string(error_message);
  } else {
    std::cout << name_count << " table(s) found:" << std::endl;
    for (size_t i = 0; i < name_count; i++) {
      std::cout << "  - " << table_names[i] << std::endl;
    }
    lancedb_free_table_names(table_names, name_count);
  }

  // Step 7: Clean up
  if (const LanceDBError result = lancedb_connection_drop_table(
      db, "registry_example", nullptr, &error_message); result != LANCEDB_SUCCESS) {
    std::cerr << "error dropping table: " << error_message << std::endl;
    lancedb_free_string(error_message);
  } else {
    std::cout << "dropped table registry_example" << std::endl;
  }

  lancedb_connection_free(db);
  lancedb_session_free(session);

  std::cout << "done" << std::endl;
  return 0;
}
