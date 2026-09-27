#include "typed_ai/profile.hpp"
#include "typed_ai/compat.hpp"

#include "duckdb/catalog/catalog_transaction.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/secret/secret_manager.hpp"

namespace duckdb {
namespace typed_ai {

namespace {

const char *SECRET_TYPE = "typed_ai";

string SecretString(const KeyValueSecret &secret, const string &key) {
	auto value = secret.TryGetValue(Id(key));
	return value.IsNull() ? "" : value.ToString();
}

double SecretNumber(const KeyValueSecret &secret, const string &key) {
	auto value = secret.TryGetValue(Id(key));
	return value.IsNull() ? -1 : value.GetValue<double>();
}

struct UrlParts {
	string scheme;
	string host;
};

//! Splits "https://api.example.com:443/v1/x" into scheme "https" and host "api.example.com".
UrlParts ParseUrl(const string &url) {
	UrlParts parts;
	auto sep = url.find("://");
	if (sep == string::npos) {
		return parts;
	}
	parts.scheme = StringUtil::Lower(url.substr(0, sep));
	auto rest = url.substr(sep + 3);
	auto end = rest.find('/');
	auto authority = rest.substr(0, end);
	authority = authority.substr(authority.find('@') == string::npos ? 0 : authority.find('@') + 1);
	if (!authority.empty() && authority[0] == '[') { // IPv6: [::1]:8080
		parts.host = authority.substr(1, authority.find(']') - 1);
	} else {
		parts.host = authority.substr(0, authority.find(':'));
	}
	parts.host = StringUtil::Lower(parts.host);
	return parts;
}

//! This computer only. An exact match, so "localhost.example.com" does not count.
bool IsLoopback(const string &host) {
	return host == "localhost" || host == "127.0.0.1" || host == "::1";
}

unique_ptr<SecretEntry> FindSecret(ClientContext &context, const string &name) {
	auto &secrets = SecretManager::Get(context);
	auto transaction = CatalogTransaction::GetSystemCatalogTransaction(context);
	if (!name.empty()) {
		auto entry = secrets.GetSecretByName(transaction, name);
		if (!entry || Str(entry->secret->GetType()) != SECRET_TYPE) {
			throw InvalidInputException(
			    "typed_ai: typed_profile is '%s', but there is no typed_ai secret with that name. "
			    "Create it with CREATE SECRET %s (TYPE typed_ai, PROVIDER 'jev', API_KEY '...')",
			    name, name);
		}
		return entry;
	}
	vector<SecretEntry> matches;
	for (auto &entry : secrets.AllSecrets(transaction)) {
		if (Str(entry.secret->GetType()) == SECRET_TYPE) {
			matches.push_back(entry);
		}
	}
	if (matches.size() > 1) {
		vector<string> names;
		for (auto &entry : matches) {
			names.push_back(Str(entry.secret->GetName()));
		}
		throw InvalidInputException(
		    "typed_ai: there are %d typed_ai secrets (%s); pick one with SET typed_profile = 'name'", matches.size(),
		    StringUtil::Join(names, ", "));
	}
	return matches.empty() ? nullptr : make_uniq<SecretEntry>(matches[0]);
}

unique_ptr<BaseSecret> CreateSecret(ClientContext &, CreateSecretInput &input) {
	auto secret = make_uniq<KeyValueSecret>(input.scope, input.type, input.provider, input.name);
	for (auto &option : input.options) {
		secret->secret_map[Id(StringUtil::Lower(option.first))] = option.second;
	}
	secret->redact_keys = {"api_key"};
	return std::move(secret);
}

} // namespace

unique_ptr<Provider> Provider::Create(const string &name) {
	if (name == "jev") {
		return CreateJevProvider();
	}
	if (name == "openai") {
		return CreateOpenAIProvider();
	}
	throw InvalidInputException("typed_ai: unknown provider '%s'; use 'jev' or 'openai'", name);
}

Profile ResolveProfile(ClientContext &context, const string &name, unique_ptr<Provider> &provider) {
	Profile profile;
	auto entry = FindSecret(context, name);
	if (entry) {
		auto &secret = dynamic_cast<const KeyValueSecret &>(*entry->secret);
		profile.name = Str(secret.GetName());
		profile.provider = Str(secret.GetProvider());
		profile.model = SecretString(secret, "model");
		profile.url = SecretString(secret, "url");
		profile.api_key = SecretString(secret, "api_key");
		profile.price = Price(SecretNumber(secret, "usd_per_mtok_in"), SecretNumber(secret, "usd_per_mtok_out"));
	} else {
		profile.name = "jev (built in)";
		profile.provider = "jev";
		auto key = std::getenv("TYPESAFE_API_KEY");
		profile.api_key = key ? key : "";
	}

	provider = Provider::Create(profile.provider);
	profile.model = profile.model.empty() ? provider->DefaultModel() : profile.model;
	profile.url = profile.url.empty() ? provider->DefaultUrl() : profile.url;

	// A built-in price is a floor: a profile priced at $0 cannot switch the budget off.
	auto builtin = provider->DefaultPrice();
	if (builtin.Known()) {
		profile.price = Price(MaxValue(builtin.in, profile.price.in), MaxValue(builtin.out, profile.price.out));
	}

	if (profile.model.empty()) {
		throw InvalidInputException("typed_ai: profile '%s' needs a model, e.g. CREATE OR REPLACE SECRET %s (TYPE "
		                            "typed_ai, PROVIDER '%s', MODEL 'gpt-4.1-mini', ...)",
		                            profile.name, profile.name, profile.provider);
	}
	if (!profile.price.Known()) {
		throw InvalidInputException("typed_ai: profile '%s' has no price, so the budget cannot be checked. Add "
		                            "USD_PER_MTOK_IN and USD_PER_MTOK_OUT to the secret (0 for a local server)",
		                            profile.name);
	}
	auto url = ParseUrl(profile.url);
	if (url.scheme != "https" && !(url.scheme == "http" && IsLoopback(url.host))) {
		throw InvalidInputException("typed_ai: profile '%s' has URL '%s'. Use https: plain http is allowed only for "
		                            "localhost, so a key or row data never crosses a network unencrypted",
		                            profile.name, profile.url);
	}
	if (profile.api_key.empty() && !IsLoopback(url.host)) {
		throw InvalidInputException("typed_ai: no API key for profile '%s'. Run CREATE SECRET (TYPE typed_ai, PROVIDER "
		                            "'%s', API_KEY '...')%s",
		                            profile.name, profile.provider,
		                            profile.provider == "jev" ? " or set TYPESAFE_API_KEY" : "");
	}
	return profile;
}

void RegisterSecretType(ExtensionLoader &loader) {
	SecretType type;
	type.name = SECRET_TYPE;
	type.deserializer = KeyValueSecret::Deserialize<KeyValueSecret>;
	type.default_provider = "jev";
	type.extension = "typed_ai";
	loader.RegisterSecretType(type);

	for (auto provider : {"jev", "openai"}) {
		CreateSecretFunction function;
		function.secret_type = SECRET_TYPE;
		function.provider = provider;
		function.function = CreateSecret;
		function.named_parameters["model"] = LogicalType::VARCHAR;
		function.named_parameters["url"] = LogicalType::VARCHAR;
		function.named_parameters["api_key"] = LogicalType::VARCHAR;
		function.named_parameters["usd_per_mtok_in"] = LogicalType::DOUBLE;
		function.named_parameters["usd_per_mtok_out"] = LogicalType::DOUBLE;
		loader.RegisterFunction(function);
	}
}

} // namespace typed_ai
} // namespace duckdb
