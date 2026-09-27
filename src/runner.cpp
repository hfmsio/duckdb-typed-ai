#include "typed_ai/runner.hpp"

#include "duckdb/common/http_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension_helper.hpp"

#include <cmath>
#include <random>
#include <thread>

namespace duckdb {
namespace typed_ai {

namespace {

struct HttpResult {
	int status = 0;
	string body;
	string transport_error;
	double retry_after_s = -1;
};

//! One POST through DuckDB's HTTP layer (httpfs supplies HTTPS and POST). DuckDB's own retries are off:
//! we retry here so we can honour Retry-After and count every attempt.
HttpResult Post(ClientContext &context, const HttpCall &call, int64_t timeout_s) {
	HttpResult result;
	try {
		auto &http = HTTPUtil::Get(*context.db);
		auto params = http.InitializeParameters(context, call.url);
		params->timeout = uint64_t(timeout_s);
		params->retries = 0;
		HTTPHeaders headers;
		for (auto &header : call.headers) {
			headers.Insert(header.first, header.second);
		}
		PostRequestInfo info(call.url, headers, *params, const_data_ptr_cast(call.body.data()), call.body.size());
		info.try_request = true;
		auto response = http.Request(info);
		if (response->HasRequestError()) {
			result.transport_error = response->GetRequestError();
			return result;
		}
		result.status = int(response->status);
		result.body = info.buffer_out.empty() ? response->body : info.buffer_out;
		if (response->HasHeader("Retry-After")) {
			result.retry_after_s = std::atof(response->GetHeaderValue("Retry-After").c_str());
		}
	} catch (std::exception &e) {
		result.transport_error = e.what();
	}
	return result;
}

//! Sleeps in short steps so Ctrl-C is not held up. Returns false when interrupted.
bool Sleep(ClientContext &context, double seconds) {
	for (double waited = 0; waited < seconds; waited += 0.05) {
		if (context.IsInterrupted()) {
			return false;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}
	return !context.IsInterrupted();
}

double Jitter() {
	thread_local std::mt19937 engine(std::random_device {}());
	return std::uniform_real_distribution<double>(0.75, 1.0)(engine);
}

struct Sent {
	Sent() : ok(false), setup_error(false) {
	}
	Sent(bool ok, bool setup_error, string error, string body)
	    : ok(ok), setup_error(setup_error), error(std::move(error)), body(std::move(body)) {
	}
	bool ok;
	//! A failure no retry can fix (bad key, bad request): it stops the query in every mode.
	bool setup_error;
	string error;
	string body;
};

Sent SendWithRetries(ClientContext &context, QueryState &qs, DatabaseState &db, const HttpCall &call) {
	auto &s = qs.settings;
	db.Count(&Counters::requests);
	for (int64_t attempt = 0;; attempt++) {
		auto r = Post(context, call, s.timeout_s);
		if (r.transport_error.empty() && r.status >= 200 && r.status < 300) {
			return Sent(true, false, "", std::move(r.body));
		}
		bool retryable = !r.transport_error.empty() || r.status == 408 || r.status == 429 || r.status >= 500;
		auto what = r.transport_error.empty()
		                ? StringUtil::Format("HTTP %d from %s: %s", r.status, call.url, r.body.substr(0, 300))
		                : StringUtil::Format("could not reach %s: %s", call.url, r.transport_error);
		if (!retryable) {
			if (r.status == 401 || r.status == 403) {
				what = StringUtil::Format("the API key was rejected (HTTP %d); check the secret for profile '%s'",
				                          r.status, qs.profile.name);
			}
			return Sent(false, true, "typed_ai: " + what, "");
		}
		if (attempt >= s.max_retries) {
			return Sent(false, false, StringUtil::Format("%s (%d attempts)", what, attempt + 1), "");
		}
		auto wait = r.retry_after_s >= 0 ? MinValue(r.retry_after_s, 30.0)
		                                 : MinValue(0.5 * std::pow(2.0, double(attempt)), 5.0) * Jitter();
		db.Count(&Counters::retries);
		if (!Sleep(context, wait)) {
			return Sent(false, false, "interrupted", "");
		}
	}
}

//! Holds one of the process-wide request slots until it goes out of scope.
struct SlotGuard {
	SlotGuard(DatabaseState &db, int64_t limit, ClientContext &context) : db(db), held(db.AcquireSlot(limit, context)) {
	}
	~SlotGuard() {
		if (held) {
			db.ReleaseSlot();
		}
	}
	DatabaseState &db;
	bool held;
};

//! Dollars with enough digits to be useful: $1.50, $0.0003.
string Money(double usd) {
	return usd >= 0.01 || usd == 0 ? StringUtil::Format("%.2f", usd) : StringUtil::Format("%.2g", usd);
}

string RowCapMessage(int64_t cap) {
	return StringUtil::Format(
	    "typed_ai: this query would send more than %d rows to the model (typed_max_rows). To allow more: "
	    "SET typed_max_rows = N. To try a few rows first, limit them before judging: "
	    "FROM (SELECT * FROM t LIMIT 10) t WHERE typed_is(...). A LIMIT after a typed_ filter still judges up to "
	    "2,048 rows, because DuckDB works in blocks.",
	    cap);
}

//! What a batch holds between asking and sending: a budget reservation and maybe the breaker's probe. Released
//! on scope exit unless the batch settled them, so no error path can leak money or wedge the breaker.
struct Claim {
	Claim(DatabaseState &db, const string &profile) : db(db), profile(profile) {
	}
	~Claim() {
		if (reserved) {
			db.Settle(estimate, 0, Usage());
		}
		if (is_probe) {
			db.DropProbe(profile);
		}
	}
	//! The batch settled its reservation and recorded its outcome itself.
	void Handled() {
		reserved = false;
		is_probe = false;
	}
	DatabaseState &db;
	const string &profile;
	double estimate = 0;
	bool reserved = false;
	bool is_probe = false;
};

//! Sends the batches of one block of rows, from one or more threads. Each step either moves a batch on or
//! marks its rows skipped with the reason; nothing here throws, so worker threads stay safe.
class BatchSender {
public:
	BatchSender(ClientContext &context, QueryState &qs, OnStop mode, const vector<Item> &items,
	            const vector<CacheKey> &keys, vector<Answer> &answers)
	    : context(context), qs(qs), db(DatabaseState::Get(context)), mode(mode), items(items), keys(keys),
	      answers(answers), profile(qs.profile), settings(qs.settings) {
	}

	void Send(const vector<idx_t> &batch) {
		try {
			SendOrSkip(batch);
		} catch (std::exception &e) {
			SetFatal(ErrorData(e).RawMessage());
			Skip(batch, "the query failed");
		}
	}

	//! A setup error from any thread; it stops the query in every mode.
	string Fatal() {
		lock_guard<mutex> guard(fatal_lock);
		return fatal;
	}

private:
	void SendOrSkip(const vector<idx_t> &batch) {
		auto reason = WhyNotSend();
		if (!reason.empty()) {
			return Skip(batch, reason);
		}
		auto batch_items = ItemsOf(batch);
		auto call = qs.provider->Build(profile, batch_items); // built before any money is reserved
		auto scale = db.EstimateScale(profile.name);
		auto estimate = EstimateUsd(qs, batch_items, scale);

		if (!db.Reserve(estimate, settings.budget_usd)) {
			return StopAndSkip(
			    batch, StringUtil::Format("budget of $%s reached (typed_budget_usd)", Money(settings.budget_usd)));
		}
		Claim claim(db, profile.name);
		claim.estimate = estimate;
		claim.reserved = true;
		if (!db.Allow(profile.name, qs.query_id, context, claim.is_probe)) {
			if (context.IsInterrupted()) {
				return Skip(batch, "the query was cancelled");
			}
			return StopAndSkip(batch,
			                   StringUtil::Format("provider failing since %d requests in a row failed (last: %s)",
			                                      settings.fail_after, db.Snapshot().last_error));
		}

		Sent sent;
		{
			SlotGuard slot(db, settings.concurrency, context);
			if (!slot.held) {
				return Skip(batch, "the query was cancelled"); // the claim releases the reservation and probe
			}
			qs.AddSent(batch.size());
			sent = SendWithRetries(context, qs, db, call);
		}
		claim.Handled(); // from here each path settles and records itself
		if (!sent.ok) {
			return OnFailed(batch, estimate, sent);
		}
		OnAnswered(batch, batch_items, estimate, scale, sent.body);
	}

	//! Empty when the batch may go out; otherwise why it may not.
	string WhyNotSend() {
		if (!Fatal().empty()) {
			return "the query failed";
		}
		if (context.IsInterrupted()) {
			return "the query was cancelled";
		}
		return qs.StopReason();
	}

	void OnFailed(const vector<idx_t> &batch, double estimate, const Sent &sent) {
		db.Settle(estimate, 0, Usage());
		db.Record(profile.name, false, settings.fail_after, qs.query_id);
		db.SetLastError(sent.error);
		if (sent.setup_error) {
			SetFatal(sent.error);
		} else {
			db.Count(&Counters::failures);
			if (mode == OnStop::ERROR) {
				qs.Stop(sent.error);
			}
		}
		Skip(batch, sent.error);
	}

	void OnAnswered(const vector<idx_t> &batch, const vector<Item> &batch_items, double estimate, double scale,
	                const string &body) {
		Usage usage;
		vector<Answer> got;
		try {
			got = qs.provider->Parse(body, batch_items, usage);
		} catch (std::exception &e) {
			db.Settle(estimate, estimate, Usage()); // it was sent, so assume it was billed
			db.Record(profile.name, true, settings.fail_after, qs.query_id);
			SetFatal(ErrorData(e).RawMessage());
			return Skip(batch, "the query failed");
		}
		auto actual = profile.price.Cost(usage);
		db.Settle(estimate, actual, usage);
		db.LearnScale(profile.name, estimate / scale, actual);
		db.Record(profile.name, true, settings.fail_after, qs.query_id);

		auto calibrated = qs.provider->Caps().calibrated;
		vector<CacheKey> paid;
		for (idx_t j = 0; j < batch.size(); j++) {
			auto i = batch[j];
			got[j].calibrated = calibrated;
			answers[i] = got[j];
			db.Store(keys[i], got[j], qs.query_id, idx_t(settings.cache_mb) << 20);
			paid.push_back(keys[i]);
		}
		qs.AddAnswered(paid);
	}

	vector<Item> ItemsOf(const vector<idx_t> &batch) {
		vector<Item> result;
		for (auto i : batch) {
			result.push_back(items[i]);
		}
		return result;
	}

	void Skip(const vector<idx_t> &batch, const string &why) {
		for (auto i : batch) {
			answers[i].error = "skipped: " + why; // each thread writes only its own rows
		}
		qs.AddSkipped(batch.size());
	}

	void StopAndSkip(const vector<idx_t> &batch, const string &reason) {
		qs.Stop(reason);
		Skip(batch, reason);
	}

	void SetFatal(const string &error) {
		lock_guard<mutex> guard(fatal_lock);
		if (fatal.empty()) {
			fatal = error;
		}
	}

	ClientContext &context;
	QueryState &qs;
	DatabaseState &db;
	OnStop mode;
	const vector<Item> &items;
	const vector<CacheKey> &keys;
	vector<Answer> &answers;
	const Profile &profile;
	const Settings &settings;
	mutex fatal_lock;
	string fatal;
};

} // namespace

vector<Answer> Run(ClientContext &context, QueryState &qs, OnStop mode, const vector<Item> &items,
                   const vector<CacheKey> &keys) {
	auto &db = DatabaseState::Get(context);
	auto &s = qs.settings;
	vector<Answer> answers(items.size());

	// Too-long inputs are refused, never cut short in secret.
	vector<idx_t> todo;
	for (idx_t i = 0; i < items.size(); i++) {
		if (int64_t(items[i].input.size()) <= s.max_input_chars) {
			todo.push_back(i);
			continue;
		}
		auto why = StringUtil::Format("input of %d characters is over typed_max_input_chars (%d)",
		                              items[i].input.size(), s.max_input_chars);
		if (mode == OnStop::ERROR) {
			throw InvalidInputException("typed_ai: %s. Shorten it, e.g. left(body, 4000), or raise the setting", why);
		}
		answers[i].error = "skipped: " + why;
		qs.AddSkipped(1);
	}

	auto stopped = qs.StopReason();
	if (!stopped.empty()) {
		for (auto i : todo) {
			answers[i].error = "skipped: " + stopped;
		}
		qs.AddSkipped(todo.size());
		return answers;
	}

	// Row cap: in 'error' mode the whole block is refused before anything is sent.
	if (mode == OnStop::ERROR && qs.Current().rows_granted + todo.size() > idx_t(s.max_rows)) {
		throw InvalidInputException(RowCapMessage(s.max_rows));
	}
	// The rows the cap still allows are sent first; the job is marked stopped after them (end of Run).
	auto granted = qs.TakeRows(todo.size());
	string cap_reason;
	if (granted < todo.size()) {
		cap_reason = StringUtil::Format("row cap of %d rows per query reached (typed_max_rows)", s.max_rows);
		for (idx_t k = granted; k < todo.size(); k++) {
			answers[todo[k]].error = "skipped: " + cap_reason;
		}
		qs.AddSkipped(todo.size() - granted);
		todo.resize(granted);
	}

	// Batches of rows that share one request.
	auto batch_size = idx_t(MinValue<int64_t>(s.batch_size, int64_t(qs.provider->Caps().max_rows_per_request)));
	vector<vector<idx_t>> batches;
	for (idx_t k = 0; k < todo.size(); k += batch_size) {
		batches.emplace_back(todo.begin() + k, todo.begin() + MinValue<idx_t>(k + batch_size, todo.size()));
	}

	if (!cap_reason.empty() && s.dry_run) {
		qs.Stop(cap_reason);
	}
	if (s.dry_run) {
		for (auto &batch : batches) {
			vector<Item> batch_items;
			for (auto i : batch) {
				batch_items.push_back(items[i]);
				answers[i].error = "dry run: no call made";
			}
			db.AddDryRun(EstimateUsd(qs, batch_items, db.EstimateScale(qs.profile.name)));
		}
		return answers;
	}
	if (batches.empty()) {
		if (!cap_reason.empty()) {
			qs.Stop(cap_reason);
		}
		return answers;
	}
	if (!context.db->ExtensionIsLoaded("httpfs")) {
		ExtensionHelper::AutoLoadExtension(context, "httpfs");
	}

	BatchSender sender(context, qs, mode, items, keys, answers);
	std::atomic<idx_t> next {0};
	auto work = [&]() {
		for (idx_t b = next++; b < batches.size(); b = next++) {
			sender.Send(batches[b]);
		}
	};
	// A profile's first request goes alone, so its real bill calibrates the estimate before requests run in parallel.
	if (!db.HasScale(qs.profile.name)) {
		sender.Send(batches[next++]);
	}
	auto workers = MinValue<idx_t>(idx_t(s.concurrency), batches.size());
	vector<std::thread> threads;
	for (idx_t t = 1; t < workers; t++) {
		threads.emplace_back(work);
	}
	work(); // this thread works too
	for (auto &thread : threads) {
		thread.join();
	}

	if (!cap_reason.empty()) {
		qs.Stop(cap_reason);
	}
	auto fatal = sender.Fatal();
	if (!fatal.empty()) {
		throw InvalidInputException(fatal);
	}
	if (context.IsInterrupted()) {
		throw InterruptException();
	}
	auto reason = qs.StopReason();
	if (mode == OnStop::ERROR && !reason.empty()) {
		throw InvalidInputException(StopMessage(context, qs, reason));
	}
	return answers;
}

} // namespace typed_ai
} // namespace duckdb
