#define DUCKDB_EXTENSION_MAIN

#include "typed_ai_extension.hpp"
#include "typed_ai/compat.hpp"
#include "typed_ai/functions.hpp"
#include "typed_ai/profile.hpp"

#include "duckdb/common/enums/set_scope.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {

//! Rejects a bad typed_on_stop when it is SET, not later when a query uses it.
static void CheckOnStop(ClientContext &, SetScope, Value &parameter) {
	auto mode = StringUtil::Lower(parameter.ToString());
	if (mode != "error" && mode != "null" && mode != "row") {
		throw InvalidInputException("typed_ai: typed_on_stop must be 'error', 'null' or 'row'; got '%s'", mode);
	}
}

static void LoadInternal(ExtensionLoader &loader) {
	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	auto global = [&](const string &name, const string &description, const Value &value) {
		config.AddExtensionOption(typed_ai::Id(name), description, value.type(), value, nullptr, SetScope::GLOBAL);
	};
	auto session = [&](const string &name, const string &description, const Value &value,
	                   set_option_callback_t check = nullptr) {
		config.AddExtensionOption(typed_ai::Id(name), description, value.type(), value, check, SetScope::SESSION);
	};

	// Limits: one value for the whole database, so a new connection or cursor cannot raise them.
	global("typed_max_rows", "Most rows one query may send to the model; cached rows are free", Value::BIGINT(1000));
	global("typed_budget_usd", "Most dollars this database may spend, counted since the extension loaded",
	       Value::DOUBLE(1.0));
	global("typed_max_input_chars", "Longest input value, in characters; longer values are refused, never cut",
	       Value::BIGINT(20000));
	global("typed_cache_mb", "Memory for saved answers", Value::BIGINT(256));
	global("typed_concurrency", "Requests in flight across the whole process", Value::BIGINT(4));

	// Knobs: per session.
	session("typed_profile",
	        "Name of the typed_ai secret to use; empty picks the only one, or Jev from TYPESAFE_API_KEY", Value(""));
	session("typed_dry_run", "Price queries and call nothing; results are NULL", Value::BOOLEAN(false));
	session("typed_on_stop", "What a stopped job shows: 'error', 'null' or 'row' ({value, error} pairs)",
	        Value("error"), CheckOnStop);
	session("typed_batch_size", "Rows per request, up to the provider's limit", Value::BIGINT(10));
	session("typed_timeout_s", "Seconds per HTTP attempt", Value::BIGINT(30));
	session("typed_max_retries", "Extra attempts after a 408, 429, 5xx or dropped connection", Value::BIGINT(3));
	session("typed_fail_after", "Failed requests in a row that stop calls for the rest of the query; 0 is off",
	        Value::BIGINT(3));

	typed_ai::RegisterSecretType(loader);
	typed_ai::RegisterFunctions(loader);
}

void TypedAiExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string TypedAiExtension::Name() {
	return "typed_ai";
}

std::string TypedAiExtension::Version() const {
#ifdef EXT_VERSION_TYPED_AI
	return EXT_VERSION_TYPED_AI;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(typed_ai, loader) {
	duckdb::LoadInternal(loader);
}
}
