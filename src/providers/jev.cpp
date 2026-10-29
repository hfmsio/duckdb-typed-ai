// TypeSafe Jev: POST /v1/systemone with one `state` and named typed questions.
// https://docs.typesafe.ai/api
#include "typed_ai/json.hpp"
#include "typed_ai/provider.hpp"

#include "duckdb/common/types/blob.hpp"

namespace duckdb {
namespace typed_ai {

namespace {

//! A row that is itself JSON (a struct or whole row) goes in as structure, anything else as text. Photos in it
//! appear by ref.
yyjson_mut_val *InputValue(JsonWriter &w, const string &input) {
	if (!input.empty() && (input[0] == '{' || input[0] == '[')) {
		JsonDoc parsed(input);
		if (parsed.doc) {
			return yyjson_val_mut_copy(w.doc, parsed.Root());
		}
	}
	return w.Str(input);
}

//! Lists each photo under its ref in `assets`; Jev swaps every place the ref appears for the picture.
void AddAssets(JsonWriter &w, yyjson_mut_val *assets, const vector<Photo> &photos) {
	for (auto &photo : photos) {
		if (yyjson_mut_obj_get(assets, photo.ref.c_str())) {
			continue;
		}
		auto asset = yyjson_mut_obj(w.doc);
		yyjson_mut_obj_add_str(w.doc, asset, "type", "image");
		yyjson_mut_obj_add(asset, w.Str("format"), w.Str(photo.image.format));
		auto &bytes = *photo.bytes;
		yyjson_mut_obj_add(asset, w.Str("data_base64"), w.Str(Blob::ToBase64(string_t(bytes.data(), bytes.size()))));
		yyjson_mut_obj_add(assets, w.Str(photo.ref), asset);
	}
}

yyjson_mut_val *QuestionJson(JsonWriter &w, const Question &q, yyjson_mut_val *instructions) {
	auto obj = yyjson_mut_obj(w.doc);
	static const char *types[] = {"noul", "choice", "score", "box"};
	yyjson_mut_obj_add_str(w.doc, obj, "type", types[uint8_t(q.kind)]);
	yyjson_mut_obj_add_val(w.doc, obj, "instructions", instructions);

	auto descriptions = w.Raw(q.criteria_json);
	if (q.kind == Kind::YES_NO) {
		if (descriptions) {
			yyjson_mut_obj_add_val(w.doc, obj, "criteria", descriptions);
		}
	} else if (q.kind == Kind::PICK) {
		auto criteria = yyjson_mut_obj(w.doc);
		for (auto &option : q.options) {
			auto description = descriptions ? yyjson_mut_obj_get(descriptions, option.c_str()) : nullptr;
			auto value = description ? yyjson_mut_val_mut_copy(w.doc, description) : yyjson_mut_null(w.doc);
			yyjson_mut_obj_add(criteria, w.Str(option), value);
		}
		yyjson_mut_obj_add_val(w.doc, obj, "criteria", criteria);
	} else if (q.kind == Kind::SCORE) {
		auto levels = yyjson_mut_arr(w.doc);
		for (auto &level : q.options) {
			yyjson_mut_arr_append(levels, w.Str(level));
		}
		yyjson_mut_obj_add_val(w.doc, obj, "criteria", levels);
	}
	return obj;
}

//! Probabilities keyed by option name (choice) or level index (score), in the question's order.
vector<double> ReadProbabilities(yyjson_val *answer, const Question &q) {
	vector<double> result;
	auto probs = Field(answer, "probabilities");
	for (idx_t i = 0; i < q.options.size(); i++) {
		auto key = q.kind == Kind::PICK ? q.options[i] : to_string(i);
		result.push_back(GetNumber(Field(probs, key.c_str()), 0));
	}
	return result;
}

class JevProvider : public Provider {
public:
	Capabilities Caps() const override {
		return {50, 255, 10, true, 300, true};
	}
	Price DefaultPrice() const override {
		return Price(0.042, 0);
	}
	string DefaultUrl() const override {
		return "https://api.typesafe.ai/v1/systemone";
	}
	string DefaultModel() const override {
		return "jev-latest";
	}

	HttpCall Build(const Profile &profile, const vector<Item> &items) const override {
		JsonWriter w;
		auto root = yyjson_mut_obj(w.doc);
		yyjson_mut_obj_add(root, w.Str("model"), w.Str(profile.model));
		auto questions = yyjson_mut_obj(w.doc);
		auto assets = yyjson_mut_obj(w.doc);

		if (items.size() == 1) {
			// One row: the row is the whole state, the question is asked plainly.
			yyjson_mut_obj_add_val(w.doc, root, "state", InputValue(w, items[0].input));
			yyjson_mut_obj_add_val(w.doc, questions, "q0",
			                       QuestionJson(w, *items[0].question, w.Str(items[0].question->text)));
		} else {
			// Several rows: each row is a named field of the state, and each question points at its row.
			auto state = yyjson_mut_obj(w.doc);
			for (idx_t i = 0; i < items.size(); i++) {
				auto row = "r" + to_string(i);
				yyjson_mut_obj_add(state, w.Str(row), InputValue(w, items[i].input));
				auto instructions = yyjson_mut_obj(w.doc);
				yyjson_mut_obj_add(instructions, w.Str("row"), w.Str("`" + row + "`"));
				yyjson_mut_obj_add(instructions, w.Str("question"), w.Str(items[i].question->text));
				yyjson_mut_obj_add(questions, w.Str("q" + to_string(i)),
				                   QuestionJson(w, *items[i].question, instructions));
			}
			yyjson_mut_obj_add_val(w.doc, root, "state", state);
		}
		yyjson_mut_obj_add_val(w.doc, root, "questions", questions);
		for (auto &item : items) {
			AddAssets(w, assets, item.photos);
			AddAssets(w, assets, item.question->photos);
		}
		bool photos = yyjson_mut_obj_size(assets) > 0;
		if (photos) {
			yyjson_mut_obj_add_val(w.doc, root, "assets", assets);
		}

		HttpCall call;
		call.url = profile.url;
		call.headers = {{"Content-Type", "application/json"}, {"Authorization", "Bearer " + profile.api_key}};
		if (photos) {
			// Jev's photo support is a preview and must be asked for on each request.
			call.headers.emplace_back("TypeSafe-Danger-Zone-Images", "true");
		}
		call.body = w.Write(root);
		return call;
	}

	vector<Answer> Parse(const string &body, const vector<Item> &items, Usage &usage) const override {
		JsonDoc parsed(body);
		auto root = parsed.doc ? parsed.Root() : nullptr;
		auto answers = Field(root, "answers");
		if (!yyjson_is_obj(answers)) {
			throw InvalidInputException("typed_ai: Jev returned no answers: %s", body.substr(0, 300));
		}
		auto model = GetString(root, "model");
		auto used = Field(root, "usage");
		usage.input_tokens = idx_t(GetNumber(Field(used, "input_tokens"), 0));
		usage.output_tokens = idx_t(GetNumber(Field(used, "output_tokens"), 0));

		vector<Answer> result;
		for (idx_t i = 0; i < items.size(); i++) {
			auto &q = *items[i].question;
			auto id = "q" + to_string(i);
			auto a = Field(answers, id.c_str());
			if (!a) {
				throw InvalidInputException("typed_ai: Jev (model %s) left out the answer to %s", model, id);
			}
			Answer answer;
			answer.model = model;
			answer.calibrated = true;
			answer.confidence = GetNumber(Field(a, "confidence"));
			if (q.kind == Kind::YES_NO) {
				answer.value = GetNumber(Field(a, "noul"));
			} else if (q.kind == Kind::BOX) {
				// No value of its own: a box with all four edges counts as the answer.
				auto box = Field(a, "box");
				answer.left = GetNumber(Field(box, "left"));
				answer.top = GetNumber(Field(box, "top"));
				answer.right = GetNumber(Field(box, "right"));
				answer.bottom = GetNumber(Field(box, "bottom"));
				bool whole = answer.left >= 0 && answer.top >= 0 && answer.right >= 0 && answer.bottom >= 0;
				answer.value = whole ? MaxValue(answer.confidence, 0.0) : -1;
			} else if (q.kind == Kind::PICK) {
				answer.choice = GetString(a, "choice");
				answer.probabilities = ReadProbabilities(a, q);
				answer.value = GetNumber(Field(Field(a, "probabilities"), answer.choice.c_str()), 0);
			} else {
				answer.value = GetNumber(Field(a, "score"));
				answer.probabilities = ReadProbabilities(a, q);
			}
			if (answer.value < 0 || (q.kind == Kind::PICK && answer.choice.empty())) {
				throw InvalidInputException("typed_ai: Jev (model %s) sent an answer without a value for %s", model,
				                            id);
			}
			result.push_back(std::move(answer));
		}
		return result;
	}
};

} // namespace

unique_ptr<Provider> CreateJevProvider() {
	return make_uniq<JevProvider>();
}

} // namespace typed_ai
} // namespace duckdb
