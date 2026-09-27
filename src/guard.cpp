#include "typed_ai/guard.hpp"
#include "typed_ai/profile.hpp"

#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"

namespace duckdb {
namespace typed_ai {

namespace {

Value GlobalSetting(ClientContext &context, const string &name) {
	Value value;
	DBConfig::GetConfig(context).TryGetCurrentSetting(name, value);
	return value;
}

Value SessionSetting(ClientContext &context, const string &name) {
	Value value;
	context.TryGetCurrentSetting(name, value);
	return value;
}

int64_t InRange(const string &name, const Value &value, int64_t low, int64_t high) {
	auto n = value.IsNull() ? low : value.GetValue<int64_t>();
	if (n < low || n > high) {
		throw InvalidInputException("typed_ai: %s must be between %d and %d; it is %d", name, low, high, n);
	}
	return n;
}

std::atomic<uint64_t> next_query_id {1};

} // namespace

OnStop Settings::ReadOnStop(ClientContext &context) {
	auto mode = StringUtil::Lower(SessionSetting(context, "typed_on_stop").ToString());
	if (mode == "error") {
		return OnStop::ERROR;
	}
	if (mode == "null") {
		return OnStop::NULLS;
	}
	if (mode == "row") {
		return OnStop::ROW;
	}
	throw InvalidInputException("typed_ai: typed_on_stop must be 'error', 'null' or 'row'; it is '%s'", mode);
}

Settings Settings::Read(ClientContext &context) {
	Settings s;
	s.max_rows =
	    InRange("typed_max_rows", GlobalSetting(context, "typed_max_rows"), 0, NumericLimits<int64_t>::Maximum());
	s.max_input_chars = InRange("typed_max_input_chars", GlobalSetting(context, "typed_max_input_chars"), 1, 10000000);
	s.budget_usd = GlobalSetting(context, "typed_budget_usd").GetValue<double>();
	s.cache_mb = InRange("typed_cache_mb", GlobalSetting(context, "typed_cache_mb"), 0, 1 << 20);
	auto profile = SessionSetting(context, "typed_profile");
	s.profile_name = profile.IsNull() ? "" : profile.ToString();
	s.dry_run = SessionSetting(context, "typed_dry_run").GetValue<bool>();
	s.on_stop = ReadOnStop(context);
	s.batch_size = InRange("typed_batch_size", SessionSetting(context, "typed_batch_size"), 1, 1000);
	s.concurrency = InRange("typed_concurrency", GlobalSetting(context, "typed_concurrency"), 1, 64);
	s.timeout_s = InRange("typed_timeout_s", SessionSetting(context, "typed_timeout_s"), 1, 600);
	s.max_retries = InRange("typed_max_retries", SessionSetting(context, "typed_max_retries"), 0, 10);
	s.fail_after = InRange("typed_fail_after", SessionSetting(context, "typed_fail_after"), 0, 1000);
	return s;
}

CacheKey CacheKey::Of(const Profile &profile, const Question &question, const string &input) {
	auto text =
	    profile.provider + '\x1d' + profile.url + '\x1d' + profile.model + '\x1d' + question.Key() + '\x1d' + input;
	auto a = Hash(text.c_str(), text.size());
	text.push_back('\x1c'); // a second, independent hash for the other 64 bits
	return {a, Hash(text.c_str(), text.size())};
}

// ---- DatabaseState ----

static shared_ptr<DatabaseState> SharedDatabaseState(ClientContext &context) {
	return ObjectCache::GetObjectCache(context).GetOrCreate<DatabaseState>(DatabaseState::ObjectType());
}

DatabaseState &DatabaseState::Get(ClientContext &context) {
	return *SharedDatabaseState(context);
}

bool DatabaseState::Reserve(double usd, double budget) {
	lock_guard<mutex> guard(lock);
	if (counters.spent_usd + counters.reserved_usd + usd > budget) {
		return false;
	}
	counters.reserved_usd += usd;
	return true;
}

void DatabaseState::Settle(double reserved, double actual, const Usage &usage) {
	lock_guard<mutex> guard(lock);
	counters.reserved_usd -= reserved;
	counters.spent_usd += actual;
	counters.input_tokens += usage.input_tokens;
	counters.output_tokens += usage.output_tokens;
}

void DatabaseState::AddDryRun(double usd) {
	lock_guard<mutex> guard(lock);
	counters.dry_run_usd += usd;
}

void DatabaseState::Count(idx_t Counters::*field, idx_t amount) {
	lock_guard<mutex> guard(lock);
	counters.*field += amount;
}

double DatabaseState::EstimateScale(const string &profile) {
	lock_guard<mutex> guard(lock);
	auto it = scales.find(profile);
	return it == scales.end() ? 1.0 : it->second;
}

bool DatabaseState::HasScale(const string &profile) {
	lock_guard<mutex> guard(lock);
	return scales.count(profile) > 0;
}

void DatabaseState::LearnScale(const string &profile, double estimate, double actual) {
	if (estimate <= 0) {
		return;
	}
	lock_guard<mutex> guard(lock);
	auto &scale = scales.emplace(profile, 1.0).first->second;
	scale = MaxValue(1.0, MaxValue(actual / estimate, 0.9 * scale));
}

void DatabaseState::SetLastError(const string &error) {
	lock_guard<mutex> guard(lock);
	counters.last_error = error;
}

Counters DatabaseState::Snapshot() {
	lock_guard<mutex> guard(lock);
	return counters;
}

bool DatabaseState::Lookup(const CacheKey &key, Answer &out) {
	lock_guard<mutex> guard(lock);
	auto it = entries.find(key);
	if (it == entries.end()) {
		return false;
	}
	out = it->second.answer;
	counters.cache_hits++;
	return true;
}

void DatabaseState::Store(const CacheKey &key, const Answer &answer, uint64_t query_id, idx_t max_bytes) {
	lock_guard<mutex> guard(lock);
	auto bytes = sizeof(Entry) + sizeof(CacheKey) * 3 + answer.choice.size() + answer.model.size() +
	             answer.probabilities.size() * sizeof(double);
	auto it = entries.find(key);
	if (it != entries.end()) {
		cache_bytes -= it->second.bytes; // replaced in place; it keeps its place in the eviction line
		it->second = Entry {answer, query_id, false, bytes};
	} else {
		entries[key] = Entry {answer, query_id, false, bytes};
		pending[query_id].push_back(key);
	}
	cache_bytes += bytes;
	cache_limit = max_bytes;
	EvictTo(max_bytes);
}

void DatabaseState::EvictTo(idx_t max_bytes) {
	// Answers of finished queries go first, oldest first.
	while (cache_bytes > max_bytes && !order.empty()) {
		auto it = entries.find(order.front());
		order.pop_front();
		if (it == entries.end() || it->second.protect) {
			continue; // already gone, or now listed in protected_order
		}
		cache_bytes -= it->second.bytes;
		entries.erase(it);
	}
	// Answers of stopped jobs go last.
	while (cache_bytes > max_bytes && !protected_order.empty()) {
		auto it = entries.find(protected_order.front());
		protected_order.pop_front();
		if (it != entries.end() && it->second.protect) {
			cache_bytes -= it->second.bytes;
			entries.erase(it);
		}
	}
}

void DatabaseState::Protect(const vector<CacheKey> &keys) {
	lock_guard<mutex> guard(lock);
	for (auto &key : keys) {
		auto it = entries.find(key);
		if (it != entries.end() && !it->second.protect) {
			it->second.protect = true;
			protected_order.push_back(key);
		}
	}
}

void DatabaseState::QueryEnded(uint64_t query_id) {
	lock_guard<mutex> guard(lock);
	for (auto &key : pending[query_id]) {
		auto it = entries.find(key);
		if (it != entries.end() && !it->second.protect) {
			order.push_back(key);
		}
	}
	pending.erase(query_id);
	EvictTo(cache_limit);
}

idx_t DatabaseState::CacheEntries() {
	lock_guard<mutex> guard(lock);
	return entries.size();
}

bool DatabaseState::Allow(const string &profile, uint64_t query_id, const std::atomic<bool> &interrupted,
                          bool &is_probe) {
	is_probe = false;
	unique_lock<mutex> guard(lock);
	while (breakers[profile].probing) {
		if (interrupted) {
			return false;
		}
		changed.wait_for(guard, std::chrono::milliseconds(100));
	}
	auto &b = breakers[profile];
	if (!b.open) {
		return true;
	}
	if (b.opened_in_query == query_id) {
		return false;
	}
	b.probing = true; // a new query gets one probe request before the breaker closes again
	is_probe = true;
	return true;
}

void DatabaseState::DropProbe(const string &profile) {
	lock_guard<mutex> guard(lock);
	breakers[profile].probing = false;
	changed.notify_all();
}

void DatabaseState::Record(const string &profile, bool ok, int64_t fail_after, uint64_t query_id) {
	lock_guard<mutex> guard(lock);
	auto &b = breakers[profile];
	bool was_probe = b.probing;
	b.probing = false;
	if (ok) {
		b.failures = 0;
		b.open = false;
	} else {
		b.failures++;
		if (was_probe || (fail_after > 0 && b.failures >= fail_after)) {
			b.open = true;
			b.opened_in_query = query_id;
		}
	}
	changed.notify_all();
}

bool DatabaseState::BreakerOpen(const string &profile) {
	lock_guard<mutex> guard(lock);
	return breakers[profile].open;
}

bool DatabaseState::AcquireSlot(int64_t limit, const std::atomic<bool> &interrupted) {
	unique_lock<mutex> guard(lock);
	while (in_flight >= limit) {
		if (interrupted) {
			return false;
		}
		changed.wait_for(guard, std::chrono::milliseconds(100));
	}
	in_flight++;
	return true;
}

void DatabaseState::ReleaseSlot() {
	lock_guard<mutex> guard(lock);
	in_flight--;
	changed.notify_all();
}

// ---- QueryState ----

QueryState &QueryState::Get(ClientContext &context) {
	return *context.registered_state->GetOrCreate<QueryState>("typed_ai");
}

QueryState::~QueryState() {
	Finish(false);
}

void QueryState::QueryBegin(ClientContext &context) {
	lock_guard<mutex> guard(lock);
	Finish(false); // a previous run that never got its QueryEnd
	current = QuerySummary();
}

void QueryState::QueryEnd(ClientContext &context, optional_ptr<ErrorData> error) {
	lock_guard<mutex> guard(lock);
	if (prepared) {
		last = current;
	}
	Finish(error || !current.stop_reason.empty()); // a stopped job's answers are kept longest, for the rerun
}

void QueryState::Finish(bool protect) {
	if (!prepared) {
		return;
	}
	if (protect) {
		db_state->Protect(paid_keys);
	}
	db_state->QueryEnded(query_id);
	paid_keys.clear();
	prepared = false;
	provider.reset();
	db_state.reset();
}

void QueryState::Prepare(ClientContext &context) {
	lock_guard<mutex> guard(lock);
	if (prepared) {
		return;
	}
	settings = Settings::Read(context);
	profile = ResolveProfile(context, settings.profile_name, provider);
	query_id = next_query_id++;
	db_state = SharedDatabaseState(context);
	prepared = true;
}

void QueryState::Stop(const string &reason) {
	lock_guard<mutex> guard(lock);
	if (current.stop_reason.empty()) {
		current.stop_reason = reason;
	}
}

string QueryState::StopReason() {
	lock_guard<mutex> guard(lock);
	return current.stop_reason;
}

idx_t QueryState::TakeRows(idx_t wanted) {
	lock_guard<mutex> guard(lock);
	auto cap = idx_t(settings.max_rows);
	auto granted = current.rows_granted >= cap ? 0 : MinValue(wanted, cap - current.rows_granted);
	current.rows_granted += granted;
	return granted;
}

void QueryState::AddSent(idx_t n) {
	lock_guard<mutex> guard(lock);
	current.rows_sent += n;
}

void QueryState::AddAnswered(const vector<CacheKey> &keys) {
	lock_guard<mutex> guard(lock);
	current.rows_answered += keys.size();
	paid_keys.insert(paid_keys.end(), keys.begin(), keys.end());
}

void QueryState::AddSkipped(idx_t n) {
	lock_guard<mutex> guard(lock);
	current.rows_skipped += n;
}

QuerySummary QueryState::Current() {
	lock_guard<mutex> guard(lock);
	return current;
}

QuerySummary QueryState::Last() {
	lock_guard<mutex> guard(lock);
	return last;
}

// ---- checks ----

double EstimateUsd(const QueryState &qs, const vector<Item> &items, double scale) {
	Usage usage;
	usage.input_tokens = qs.provider->Caps().overhead_tokens_per_request;
	for (auto &item : items) {
		idx_t chars = item.input.size() + item.question->text.size() + item.question->criteria_json.size();
		for (auto &option : item.question->options) {
			chars += option.size() + 4;
		}
		usage.input_tokens += chars / 4 + 1;
		usage.output_tokens += 2;
	}
	return qs.profile.price.Cost(usage) * scale;
}

void CheckQuestion(const Question &q, const Capabilities &caps, const string &provider) {
	if (q.text.empty()) {
		throw InvalidInputException("typed_ai: the question is empty");
	}
	auto with = provider.empty() ? "" : " with provider '" + provider + "'";
	if (q.kind == Kind::PICK && (q.options.size() < 2 || q.options.size() > caps.max_options)) {
		throw InvalidInputException("typed_ai: typed_pick needs 2 to %d options%s; it got %d", caps.max_options, with,
		                            q.options.size());
	}
	if (q.kind == Kind::SCORE && (q.options.size() < 2 || q.options.size() > caps.max_levels)) {
		throw InvalidInputException("typed_ai: typed_score needs 2 to %d levels%s; it got %d", caps.max_levels, with,
		                            q.options.size());
	}
}

string StopMessage(ClientContext &context, QueryState &qs, const string &reason) {
	auto summary = qs.Current();
	auto spent = DatabaseState::Get(context).Snapshot().spent_usd;
	return StringUtil::Format("typed_ai: %s. %d rows were answered and saved in this query, and $%.4f has been spent "
	                          "in total. Run the query again to continue: saved answers cost nothing. Requests already "
	                          "in flight when it stopped may still be billed.",
	                          reason, summary.rows_answered, spent);
}

} // namespace typed_ai
} // namespace duckdb
