#pragma once

#include "duckdb/main/client_context_state.hpp"
#include "duckdb/storage/object_cache.hpp"
#include "typed_ai/provider.hpp"

#include <condition_variable>
#include <deque>

namespace duckdb {
namespace typed_ai {

//! What a job shows when it stops partway: an error, NULL rows, or {value, error} pairs.
enum class OnStop : uint8_t { ERROR, NULLS, ROW };

//! Every setting, read once when a query first calls a typed_ function.
struct Settings {
	// Database-wide limits: read from the global value, so SET SESSION cannot raise them.
	int64_t max_rows;
	int64_t max_input_chars;
	double budget_usd;
	int64_t cache_mb;
	int64_t concurrency;
	// Per session.
	string profile_name;
	bool dry_run;
	OnStop on_stop;
	int64_t batch_size;
	int64_t timeout_s;
	int64_t max_retries;
	int64_t fail_after;

	static Settings Read(ClientContext &context);
	static OnStop ReadOnStop(ClientContext &context);
};

//! 128-bit fingerprint of (profile, model, question, input). The text itself is never stored.
struct CacheKey {
	hash_t a;
	hash_t b;
	bool operator==(const CacheKey &o) const {
		return a == o.a && b == o.b;
	}
	static CacheKey Of(const Profile &profile, const Question &question, const string &input);
};

struct CacheKeyHash {
	size_t operator()(const CacheKey &k) const {
		return k.a;
	}
};

struct Counters {
	double spent_usd = 0;
	double reserved_usd = 0;
	double dry_run_usd = 0;
	idx_t requests = 0;
	idx_t retries = 0;
	idx_t failures = 0;
	idx_t input_tokens = 0;
	idx_t output_tokens = 0;
	idx_t cache_hits = 0;
	string last_error;
};

//! One per database, shared by every connection and cursor: spend, answer cache, breaker, in-flight limit.
class DatabaseState : public ObjectCacheEntry {
public:
	static DatabaseState &Get(ClientContext &context);
	static string ObjectType() {
		return "typed_ai_state";
	}
	string GetObjectType() override {
		return ObjectType();
	}
	optional_idx GetEstimatedCacheMemory() const override {
		return optional_idx(); // never evicted by DuckDB
	}

	// Money: a request reserves its estimate first, then settles to the real cost.
	bool Reserve(double usd, double budget);
	void Settle(double reserved, double actual, const Usage &usage);
	void AddDryRun(double usd);
	void Count(idx_t Counters::*field, idx_t amount = 1);
	//! How far real bills ran over the estimate for this profile, at least 1. Rises at once, eases off slowly.
	double EstimateScale(const string &profile);
	bool HasScale(const string &profile);
	void LearnScale(const string &profile, double estimate, double actual);
	void SetLastError(const string &error);
	Counters Snapshot();

	// Answer cache. Answers from running queries are never evicted; answers from stopped jobs go last.
	bool Lookup(const CacheKey &key, Answer &out);
	void Store(const CacheKey &key, const Answer &answer, uint64_t query_id, idx_t max_bytes);
	void Protect(const vector<CacheKey> &keys);
	//! Lets a finished query's answers be evicted, and evicts down to the limit.
	void QueryEnded(uint64_t query_id);
	idx_t CacheEntries();

	// Breaker, per profile. Allow() waits while another thread's probe request is out; false when the breaker is
	// open or the query was cancelled while waiting. `is_probe` says this caller now owns the one probe request.
	bool Allow(const string &profile, uint64_t query_id, ClientContext &context, bool &is_probe);
	//! Gives up a probe that was never sent, so other threads stop waiting for it.
	void DropProbe(const string &profile);
	void Record(const string &profile, bool ok, int64_t fail_after, uint64_t query_id);
	bool BreakerOpen(const string &profile);

	// Requests in flight across the whole process. False when the query was cancelled while waiting.
	bool AcquireSlot(int64_t limit, ClientContext &context);
	void ReleaseSlot();

private:
	struct Entry {
		Answer answer;
		uint64_t query_id;
		bool protect;
		idx_t bytes;
	};
	struct Breaker {
		int64_t failures = 0;
		bool open = false;
		uint64_t opened_in_query = 0;
		bool probing = false;
	};

	void EvictTo(idx_t max_bytes);

	mutex lock;
	std::condition_variable changed;
	Counters counters;
	unordered_map<CacheKey, Entry, CacheKeyHash> entries;
	//! Eviction lines, oldest first. A running query's answers wait in `pending` and join `order` when it ends,
	//! so they cannot be evicted and are never rescanned.
	std::deque<CacheKey> order;
	std::deque<CacheKey> protected_order;
	unordered_map<uint64_t, vector<CacheKey>> pending;
	idx_t cache_bytes = 0;
	idx_t cache_limit = 0;
	unordered_map<string, Breaker> breakers;
	unordered_map<string, double> scales;
	int64_t in_flight = 0;
};

//! Summary of one query's typed_ai work, kept for typed_usage().
struct QuerySummary {
	//! Rows let through by the row cap; a row can be let through and still skipped later, e.g. for budget.
	idx_t rows_granted = 0;
	//! Rows that went out in a request.
	idx_t rows_sent = 0;
	idx_t rows_answered = 0;
	idx_t rows_skipped = 0;
	string stop_reason;
};

//! One per connection. Holds what the running query has done, and the last finished query's summary.
class QueryState : public ClientContextState {
public:
	static QueryState &Get(ClientContext &context);

	~QueryState() override;
	void QueryBegin(ClientContext &context) override;
	void QueryEnd(ClientContext &context, optional_ptr<ErrorData> error) override;

	//! Reads settings and resolves the profile on the first call in a query.
	void Prepare(ClientContext &context);

	//! Marks the job stopped; later rows are skipped with this reason and cost nothing.
	void Stop(const string &reason);
	string StopReason();
	//! Takes up to `wanted` rows from the per-query cap and returns how many were granted.
	idx_t TakeRows(idx_t wanted);
	void AddSent(idx_t n);
	void AddAnswered(const vector<CacheKey> &keys);
	void AddSkipped(idx_t n);
	QuerySummary Current();
	QuerySummary Last();

	uint64_t query_id = 0;
	Settings settings;
	Profile profile;
	unique_ptr<Provider> provider;

private:
	//! Lets this query's answers be evicted. Also runs from QueryBegin and the destructor, so answers are never
	//! stuck if DuckDB skips a QueryEnd.
	void Finish(bool protect);

	mutex lock;
	bool prepared = false;
	shared_ptr<DatabaseState> db_state;
	QuerySummary current;
	QuerySummary last;
	vector<CacheKey> paid_keys;
};

//! Rough cost in dollars of sending these items in one request, scaled up by what real bills taught us.
double EstimateUsd(const QueryState &qs, const vector<Item> &items, double scale);

//! Checks a question against the provider's limits; throws with the allowed range.
void CheckQuestion(const Question &q, const Capabilities &caps, const string &provider);

//! The message that ends a stopped job in 'error' mode.
string StopMessage(ClientContext &context, QueryState &qs, const string &reason);

} // namespace typed_ai
} // namespace duckdb
