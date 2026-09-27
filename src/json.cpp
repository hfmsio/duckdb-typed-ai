#include "typed_ai/compat.hpp"
#include "typed_ai/json.hpp"
#include "typed_ai/types.hpp"

namespace duckdb {
namespace typed_ai {

JsonDoc::JsonDoc(const string &text) {
	doc = yyjson_read(text.c_str(), text.size(), 0);
}

JsonDoc::~JsonDoc() {
	if (doc) {
		yyjson_doc_free(doc);
	}
}

JsonWriter::JsonWriter() : doc(yyjson_mut_doc_new(nullptr)) {
}

JsonWriter::~JsonWriter() {
	yyjson_mut_doc_free(doc);
}

yyjson_mut_val *JsonWriter::Str(const string &s) {
	return yyjson_mut_strncpy(doc, s.c_str(), s.size());
}

yyjson_mut_val *JsonWriter::Raw(const string &json) {
	if (json.empty()) {
		return nullptr;
	}
	JsonDoc parsed(json);
	if (!parsed.doc) {
		throw InvalidInputException("typed_ai: not valid JSON: %s", json);
	}
	return yyjson_val_mut_copy(doc, parsed.Root());
}

string JsonWriter::Write(yyjson_mut_val *root) {
	yyjson_mut_doc_set_root(doc, root);
	size_t len = 0;
	char *text = yyjson_mut_write(doc, 0, &len);
	string result(text, len);
	free(text);
	return result;
}

yyjson_mut_val *ValueToJson(JsonWriter &w, const Value &value) {
	if (value.IsNull()) {
		return yyjson_mut_null(w.doc);
	}
	auto &type = value.type();
	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
		return yyjson_mut_bool(w.doc, value.GetValue<bool>());
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
		return yyjson_mut_sint(w.doc, value.GetValue<int64_t>());
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
		return yyjson_mut_uint(w.doc, value.GetValue<uint64_t>());
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
	case LogicalTypeId::DECIMAL:
		return yyjson_mut_real(w.doc, value.GetValue<double>());
	case LogicalTypeId::STRUCT: {
		auto obj = yyjson_mut_obj(w.doc);
		auto &children = StructValue::GetChildren(value);
		for (idx_t i = 0; i < children.size(); i++) {
			yyjson_mut_obj_add(obj, w.Str(Str(StructType::GetChildName(type, i))), ValueToJson(w, children[i]));
		}
		return obj;
	}
	case LogicalTypeId::LIST:
	case LogicalTypeId::ARRAY: {
		auto arr = yyjson_mut_arr(w.doc);
		auto &children =
		    type.id() == LogicalTypeId::LIST ? ListValue::GetChildren(value) : ArrayValue::GetChildren(value);
		for (auto &child : children) {
			yyjson_mut_arr_append(arr, ValueToJson(w, child));
		}
		return arr;
	}
	default:
		// Text, dates, times, UUIDs, maps and the rest read best as DuckDB prints them.
		return w.Str(value.ToString());
	}
}

string InputText(const Value &value) {
	if (value.type().id() == LogicalTypeId::VARCHAR) {
		return StringValue::Get(value);
	}
	JsonWriter w;
	return w.Write(ValueToJson(w, value));
}

yyjson_val *Field(yyjson_val *obj, const char *key) {
	return yyjson_is_obj(obj) ? yyjson_obj_get(obj, key) : nullptr;
}

string GetString(yyjson_val *obj, const char *key, const string &fallback) {
	auto val = Field(obj, key);
	return yyjson_is_str(val) ? string(yyjson_get_str(val), yyjson_get_len(val)) : fallback;
}

double GetNumber(yyjson_val *val, double fallback) {
	return yyjson_is_num(val) ? yyjson_get_num(val) : fallback;
}

string Question::Key() const {
	string key = to_string(uint8_t(kind)) + '\x1f' + text + '\x1f';
	for (auto &option : options) {
		key += option + '\x1e';
	}
	return key + '\x1f' + criteria_json;
}

string Answer::ToJson(const Question &question) const {
	JsonWriter w;
	auto obj = yyjson_mut_obj(w.doc);
	static const char *kind_names[] = {"yes_no", "pick", "score"};
	yyjson_mut_obj_add_str(w.doc, obj, "type", kind_names[uint8_t(question.kind)]);
	if (!error.empty()) {
		yyjson_mut_obj_add(obj, w.Str("error"), w.Str(error));
		return w.Write(obj);
	}
	if (question.kind == Kind::YES_NO) {
		yyjson_mut_obj_add_real(w.doc, obj, "probability", value);
	} else {
		if (question.kind == Kind::PICK) {
			yyjson_mut_obj_add(obj, w.Str("choice"), w.Str(choice));
		} else {
			yyjson_mut_obj_add_real(w.doc, obj, "score", value);
		}
		auto probs = yyjson_mut_obj(w.doc);
		for (idx_t i = 0; i < probabilities.size() && i < question.options.size(); i++) {
			yyjson_mut_obj_add(probs, w.Str(question.options[i]), yyjson_mut_real(w.doc, probabilities[i]));
		}
		yyjson_mut_obj_add_val(w.doc, obj, "probabilities", probs);
		if (confidence >= 0) {
			yyjson_mut_obj_add_real(w.doc, obj, "confidence", confidence);
		}
	}
	yyjson_mut_obj_add(obj, w.Str("model"), w.Str(model));
	yyjson_mut_obj_add_bool(w.doc, obj, "calibrated", calibrated);
	yyjson_mut_obj_add_null(w.doc, obj, "error");
	return w.Write(obj);
}

} // namespace typed_ai
} // namespace duckdb
