#pragma once

#include "typed_ai/guard.hpp"

namespace duckdb {
namespace typed_ai {

//! Answers items that missed the cache: checks limits, batches, sends, retries and saves each answer.
//! A row that was not answered gets an Answer whose `error` says why. Throws for setup errors in every mode,
//! and for any stop in 'error' mode. `mode` is the one fixed when the query was planned.
vector<Answer> Run(ClientContext &context, QueryState &qs, OnStop mode, const vector<Item> &items,
                   const vector<CacheKey> &keys);

} // namespace typed_ai
} // namespace duckdb
