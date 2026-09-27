#pragma once

#include "duckdb.hpp"

namespace duckdb {
namespace typed_ai {

//! The three kinds of question every provider must answer.
enum class Kind : uint8_t { YES_NO, PICK, SCORE };

//! One typed question, in typed_ai's own provider-neutral shape.
struct Question {
	Kind kind = Kind::YES_NO;
	string text;
	//! Options for PICK, ordered levels for SCORE.
	vector<string> options;
	//! Optional JSON object: {"true": .., "false": ..} for YES_NO, {option: description} for PICK.
	string criteria_json;

	//! Stable text that identifies the question, used in the cache key.
	string Key() const;
};

//! One row to judge: the input text and the question asked about it.
struct Item {
	Item(string input, const Question *question) : input(std::move(input)), question(question) {
	}
	string input;
	const Question *question;
};

//! A provider's answer to one item. `error` is empty on success.
struct Answer {
	//! YES_NO: probability of yes. SCORE: probability-weighted level. PICK: probability of `choice`.
	double value = 0;
	string choice;
	//! Probability per option or level, in the question's order. Empty for YES_NO.
	vector<double> probabilities;
	//! -1 when the provider reports none.
	double confidence = -1;
	string model;
	bool calibrated = false;
	string error;

	string ToJson(const Question &question) const;
};

struct Usage {
	idx_t input_tokens = 0;
	idx_t output_tokens = 0;
};

//! US dollars per million tokens. Negative means unknown.
struct Price {
	Price() : in(-1), out(-1) {
	}
	Price(double in, double out) : in(in), out(out) {
	}
	double in;
	double out;

	bool Known() const {
		return in >= 0 && out >= 0;
	}
	double Cost(const Usage &usage) const {
		return (double(usage.input_tokens) * in + double(usage.output_tokens) * out) / 1e6;
	}
};

//! What a provider can do. Functions check these before any call.
struct Capabilities {
	idx_t max_rows_per_request;
	idx_t max_options;
	idx_t max_levels;
	bool calibrated;
	//! Fixed input tokens each request adds on top of the rows, for the cost estimate.
	idx_t overhead_tokens_per_request;
};

//! Everything needed to talk to one provider. Built from a DuckDB secret.
struct Profile {
	string name;
	string provider;
	string model;
	string url;
	string api_key;
	Price price;
};

struct HttpCall {
	string url;
	vector<pair<string, string>> headers;
	string body;
};

} // namespace typed_ai
} // namespace duckdb
