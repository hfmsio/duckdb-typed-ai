#pragma once

#include "duckdb.hpp"
#include "yyjson.hpp"

namespace duckdb {
namespace typed_ai {

using namespace duckdb_yyjson; // NOLINT

//! Owns a parsed JSON document and frees it when it goes out of scope.
struct JsonDoc {
	explicit JsonDoc(const string &text);
	~JsonDoc();
	JsonDoc(const JsonDoc &) = delete;
	JsonDoc &operator=(const JsonDoc &) = delete;

	yyjson_doc *doc = nullptr;
	yyjson_val *Root() const {
		return yyjson_doc_get_root(doc);
	}
};

//! Owns a mutable JSON document being built.
struct JsonWriter {
	JsonWriter();
	~JsonWriter();
	JsonWriter(const JsonWriter &) = delete;
	JsonWriter &operator=(const JsonWriter &) = delete;

	yyjson_mut_doc *doc;

	yyjson_mut_val *Str(const string &s);
	//! Parses `json` into this document; returns null when it is empty.
	yyjson_mut_val *Raw(const string &json);
	string Write(yyjson_mut_val *root);
};

//! A DuckDB value as JSON: structs become objects, lists become arrays, so column names reach the model.
yyjson_mut_val *ValueToJson(JsonWriter &w, const Value &value);

//! The text a row is judged on: strings as they are, anything else as JSON.
string InputText(const Value &value);

// Small readers that return a fallback when the field is missing or has the wrong type.
yyjson_val *Field(yyjson_val *obj, const char *key);
string GetString(yyjson_val *obj, const char *key, const string &fallback = "");
double GetNumber(yyjson_val *val, double fallback = -1);

} // namespace typed_ai
} // namespace duckdb
