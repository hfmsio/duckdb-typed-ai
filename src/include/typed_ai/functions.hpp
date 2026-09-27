#pragma once

#include "duckdb.hpp"

namespace duckdb {
class ExtensionLoader;

namespace typed_ai {

//! typed_is, typed_prob, typed_pick, typed_score, typed_ask and typed_usage().
void RegisterFunctions(ExtensionLoader &loader);

} // namespace typed_ai
} // namespace duckdb
