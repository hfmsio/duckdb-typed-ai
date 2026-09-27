#include "typed_ai/functions.hpp"
#include "typed_ai/json.hpp"
#include "typed_ai/profile.hpp"
#include "typed_ai/runner.hpp"

#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"

namespace duckdb {
namespace typed_ai {

namespace {

enum class Shape : uint8_t { IS, PROB, PICK, SCORE, ASK };

struct TypedBindData : public FunctionData {
	TypedBindData(Shape shape, OnStop mode) : shape(shape), mode(mode) {
	}
	Shape shape;
	//! Fixed when the query is planned, because it decides the return type.
	OnStop mode;

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<TypedBindData>(shape, mode);
	}
	bool Equals(const FunctionData &other) const override {
		auto &o = other.Cast<TypedBindData>();
		return shape == o.shape && mode == o.mode;
	}
};

Kind KindOf(Shape shape) {
	return shape == Shape::PICK ? Kind::PICK : shape == Shape::SCORE ? Kind::SCORE : Kind::YES_NO;
}

//! typed_ask's question: {"v": 1, "type": "yes_no"|"pick"|"score", "question": "..", "options"|"levels": [..],
//! "criteria": {..}}
Question ParseAsk(const string &json) {
	JsonDoc parsed(json);
	auto root = parsed.doc ? parsed.Root() : nullptr;
	if (!yyjson_is_obj(root)) {
		throw InvalidInputException("typed_ai: typed_ask needs a JSON object, e.g. {\"v\": 1, \"type\": \"yes_no\", "
		                            "\"question\": \"...\"}; got: %s",
		                            json.substr(0, 200));
	}
	if (GetNumber(Field(root, "v"), 1) != 1) {
		throw InvalidInputException("typed_ai: typed_ask question version must be 1");
	}
	Question q;
	auto type = GetString(root, "type");
	if (type == "yes_no") {
		q.kind = Kind::YES_NO;
	} else if (type == "pick") {
		q.kind = Kind::PICK;
	} else if (type == "score") {
		q.kind = Kind::SCORE;
	} else {
		throw InvalidInputException(
		    "typed_ai: typed_ask \"type\" must be \"yes_no\", \"pick\" or \"score\"; got \"%s\"", type);
	}
	q.text = GetString(root, "question");
	auto list = Field(root, q.kind == Kind::SCORE ? "levels" : "options");
	size_t idx, max;
	yyjson_val *option;
	yyjson_arr_foreach(list, idx, max, option) {
		q.options.push_back(yyjson_is_str(option) ? yyjson_get_str(option) : "");
	}
	auto criteria = Field(root, "criteria");
	if (criteria) {
		size_t len;
		auto text = yyjson_val_write(criteria, 0, &len);
		q.criteria_json = string(text, len);
		free(text);
	}
	return q;
}

Question QuestionAt(const TypedBindData &bind, DataChunk &args, idx_t row) {
	if (bind.shape == Shape::ASK) {
		return ParseAsk(args.GetValue(1, row).ToString());
	}
	Question q;
	q.kind = KindOf(bind.shape);
	q.text = args.GetValue(1, row).ToString();
	if (q.kind != Kind::YES_NO) {
		auto list = args.GetValue(2, row); // keep it alive while we read its children
		for (auto &option : ListValue::GetChildren(list)) {
			if (option.IsNull()) {
				throw InvalidInputException("typed_ai: the options list contains NULL");
			}
			q.options.push_back(option.ToString());
		}
	}
	return q;
}

Value ResultValue(const TypedBindData &bind, const LogicalType &type, const Answer &a, double threshold) {
	bool answered = a.error.empty();
	Value value;
	switch (bind.shape) {
	case Shape::IS:
		return answered ? Value::BOOLEAN(a.value >= threshold) : Value(type);
	case Shape::PICK:
		value = answered ? Value(a.choice) : Value(LogicalType::VARCHAR);
		break;
	default:
		value = answered ? Value::DOUBLE(a.value) : Value(LogicalType::DOUBLE);
		break;
	}
	if (bind.mode != OnStop::ROW) {
		return value;
	}
	child_list_t<Value> pair {{"value", value}, {"error", answered ? Value(LogicalType::VARCHAR) : Value(a.error)}};
	return Value::STRUCT(std::move(pair));
}

void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &bind = state.expr.Cast<BoundFunctionExpression>().bind_info->Cast<TypedBindData>();
	auto &context = state.GetContext();
	auto rows = args.size();
	vector<bool> is_null(rows, false);
	bool any_value = false;
	for (idx_t row = 0; row < rows; row++) {
		for (idx_t col = 0; col < args.ColumnCount(); col++) {
			is_null[row] = is_null[row] || args.GetValue(col, row).IsNull();
		}
		any_value = any_value || !is_null[row];
	}
	if (!any_value) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
		ConstantVector::SetNull(result, true); // NULL in, NULL out: no profile or key needed
		return;
	}

	auto &qs = QueryState::Get(context);
	qs.Prepare(context);
	auto &db = DatabaseState::Get(context);
	auto caps = qs.provider->Caps();
	vector<Question> questions(rows);
	vector<Answer> answers(rows);
	vector<idx_t> misses;
	vector<Item> items;
	vector<CacheKey> keys;

	for (idx_t row = 0; row < rows; row++) {
		if (is_null[row]) {
			continue; // NULL in, NULL out, no call
		}
		questions[row] = QuestionAt(bind, args, row);
		CheckQuestion(questions[row], caps, qs.profile.provider);
		auto input = InputText(args.GetValue(0, row));
		auto key = CacheKey::Of(qs.profile, questions[row], input);
		if (db.Lookup(key, answers[row])) {
			continue;
		}
		misses.push_back(row);
		items.emplace_back(std::move(input), &questions[row]);
		keys.push_back(key);
	}

	auto fresh = Run(context, qs, bind.mode, items, keys);
	for (idx_t k = 0; k < misses.size(); k++) {
		answers[misses[k]] = std::move(fresh[k]);
	}

	auto &type = result.GetType();
	for (idx_t row = 0; row < rows; row++) {
		if (is_null[row]) {
			result.SetValue(row, Value(type));
		} else if (bind.shape == Shape::ASK) {
			FlatVector::GetData<string_t>(result)[row] =
			    StringVector::AddString(result, answers[row].ToJson(questions[row]));
		} else {
			double threshold =
			    args.ColumnCount() > 2 && bind.shape == Shape::IS ? args.GetValue(2, row).GetValue<double>() : 0.5;
			if (threshold < 0 || threshold > 1) {
				throw InvalidInputException("typed_ai: the threshold must be between 0 and 1; it is %f", threshold);
			}
			result.SetValue(row, ResultValue(bind, type, answers[row], threshold));
		}
	}
}

LogicalType ReturnType(Shape shape, OnStop mode) {
	if (shape == Shape::IS) {
		return LogicalType::BOOLEAN;
	}
	if (shape == Shape::ASK) {
		return LogicalType::JSON();
	}
	auto value = shape == Shape::PICK ? LogicalType::VARCHAR : LogicalType::DOUBLE;
	if (mode != OnStop::ROW) {
		return value;
	}
	return LogicalType::STRUCT({{"value", value}, {"error", LogicalType::VARCHAR}});
}

//! Checks constant arguments while the query is planned, before any call.
void CheckConstants(ClientContext &context, Shape shape, vector<unique_ptr<Expression>> &arguments) {
	auto constant = [&](idx_t i) {
		return arguments.size() > i && arguments[i]->IsFoldable()
		           ? ExpressionExecutor::EvaluateScalar(context, *arguments[i])
		           : Value();
	};
	if (shape == Shape::ASK) {
		auto json = constant(1);
		if (!json.IsNull()) {
			ParseAsk(json.ToString());
		}
		return;
	}
	if (shape == Shape::PICK || shape == Shape::SCORE) {
		auto options = constant(2);
		if (!options.IsNull()) {
			Question q;
			q.kind = KindOf(shape);
			q.text = "?";
			for (auto &option : ListValue::GetChildren(options)) {
				q.options.push_back(option.ToString());
			}
			CheckQuestion(q, {1, 255, 10, false, 0}, "");
		}
	}
	if (shape == Shape::IS) {
		auto threshold = constant(2);
		if (!threshold.IsNull() && (threshold.GetValue<double>() < 0 || threshold.GetValue<double>() > 1)) {
			throw InvalidInputException("typed_ai: the threshold must be between 0 and 1");
		}
	}
}

template <Shape SHAPE>
unique_ptr<FunctionData> Bind(ClientContext &context, ScalarFunction &function,
                              vector<unique_ptr<Expression>> &arguments) {
	function.arguments[0] = arguments[0]->return_type; // any input type; rows arrive as structs
	auto mode = Settings::ReadOnStop(context);
	function.SetReturnType(ReturnType(SHAPE, mode));
	CheckConstants(context, SHAPE, arguments);
	return make_uniq<TypedBindData>(SHAPE, mode);
}

template <Shape SHAPE>
ScalarFunction Make(const string &name, vector<LogicalType> arguments) {
	// CONSISTENT (the default) and never VOLATILE: VOLATILE made ORDER BY .. LIMIT 10 judge every row.
	ScalarFunction function(name, std::move(arguments), LogicalType::ANY, Execute, Bind<SHAPE>);
	function.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	return function;
}

// ---- typed_usage() ----

struct UsageState : public GlobalTableFunctionState {
	bool done = false;
};

unique_ptr<FunctionData> UsageBind(ClientContext &, TableFunctionBindInput &, vector<LogicalType> &types,
                                   vector<string> &names) {
	auto add = [&](const string &name, const LogicalType &type) {
		names.push_back(name);
		types.push_back(type);
	};
	add("profile", LogicalType::VARCHAR);
	add("provider", LogicalType::VARCHAR);
	add("model", LogicalType::VARCHAR);
	add("spent_usd", LogicalType::DOUBLE);
	add("budget_usd", LogicalType::DOUBLE);
	add("dry_run_usd", LogicalType::DOUBLE);
	add("requests", LogicalType::BIGINT);
	add("retries", LogicalType::BIGINT);
	add("failures", LogicalType::BIGINT);
	add("input_tokens", LogicalType::BIGINT);
	add("output_tokens", LogicalType::BIGINT);
	add("cache_hits", LogicalType::BIGINT);
	add("cache_entries", LogicalType::BIGINT);
	add("breaker", LogicalType::VARCHAR);
	add("last_query_rows_sent", LogicalType::BIGINT);
	add("last_query_answered", LogicalType::BIGINT);
	add("last_query_skipped", LogicalType::BIGINT);
	add("last_query_stop_reason", LogicalType::VARCHAR);
	add("last_error", LogicalType::VARCHAR);
	return make_uniq<TableFunctionData>();
}

unique_ptr<GlobalTableFunctionState> UsageInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<UsageState>();
}

void UsageScan(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &state = input.global_state->Cast<UsageState>();
	if (state.done) {
		return;
	}
	state.done = true;
	auto &db = DatabaseState::Get(context);
	auto counters = db.Snapshot();
	auto settings = Settings::Read(context);
	auto last = QueryState::Get(context).Last();

	Profile profile;
	unique_ptr<Provider> provider;
	try {
		profile = ResolveProfile(context, settings.profile_name, provider);
	} catch (std::exception &) {
		// typed_usage() still reports spend when no profile is usable yet.
	}
	auto text = [](const string &s) {
		return s.empty() ? Value() : Value(s);
	};
	vector<Value> row {text(profile.name),
	                   text(profile.provider),
	                   text(profile.model),
	                   Value::DOUBLE(counters.spent_usd),
	                   Value::DOUBLE(settings.budget_usd),
	                   Value::DOUBLE(counters.dry_run_usd),
	                   Value::BIGINT(int64_t(counters.requests)),
	                   Value::BIGINT(int64_t(counters.retries)),
	                   Value::BIGINT(int64_t(counters.failures)),
	                   Value::BIGINT(int64_t(counters.input_tokens)),
	                   Value::BIGINT(int64_t(counters.output_tokens)),
	                   Value::BIGINT(int64_t(counters.cache_hits)),
	                   Value::BIGINT(int64_t(db.CacheEntries())),
	                   Value(profile.name.empty()           ? "unknown"
	                         : db.BreakerOpen(profile.name) ? "open"
	                                                        : "closed"),
	                   Value::BIGINT(int64_t(last.rows_sent)),
	                   Value::BIGINT(int64_t(last.rows_answered)),
	                   Value::BIGINT(int64_t(last.rows_skipped)),
	                   text(last.stop_reason),
	                   text(counters.last_error)};
	for (idx_t col = 0; col < row.size(); col++) {
		output.SetValue(col, 0, row[col]);
	}
	output.SetCardinality(1);
}

} // namespace

void RegisterFunctions(ExtensionLoader &loader) {
	auto list = LogicalType::LIST(LogicalType::VARCHAR);
	ScalarFunctionSet is_set("typed_is");
	is_set.AddFunction(Make<Shape::IS>("typed_is", {LogicalType::ANY, LogicalType::VARCHAR}));
	is_set.AddFunction(Make<Shape::IS>("typed_is", {LogicalType::ANY, LogicalType::VARCHAR, LogicalType::DOUBLE}));
	loader.RegisterFunction(is_set);
	loader.RegisterFunction(Make<Shape::PROB>("typed_prob", {LogicalType::ANY, LogicalType::VARCHAR}));
	loader.RegisterFunction(Make<Shape::PICK>("typed_pick", {LogicalType::ANY, LogicalType::VARCHAR, list}));
	loader.RegisterFunction(Make<Shape::SCORE>("typed_score", {LogicalType::ANY, LogicalType::VARCHAR, list}));
	loader.RegisterFunction(Make<Shape::ASK>("typed_ask", {LogicalType::ANY, LogicalType::VARCHAR}));
	loader.RegisterFunction(TableFunction("typed_usage", {}, UsageScan, UsageBind, UsageInit));
}

} // namespace typed_ai
} // namespace duckdb
