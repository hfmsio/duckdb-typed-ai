#pragma once

#include "typed_ai/types.hpp"

namespace duckdb {
namespace typed_ai {

//! The seam every provider sits behind. A provider only builds request text and reads response text;
//! it never touches the network, so it can be tested with saved JSON.
class Provider {
public:
	virtual ~Provider() = default;

	virtual Capabilities Caps() const = 0;
	//! Price.Known() is false when the provider has no public price table.
	virtual Price DefaultPrice() const = 0;
	virtual string DefaultUrl() const = 0;
	//! Empty when the profile must name a model.
	virtual string DefaultModel() const = 0;

	virtual HttpCall Build(const Profile &profile, const vector<Item> &items) const = 0;
	//! One answer per item, in order. Throws InvalidInputException when the body is not a valid answer.
	virtual vector<Answer> Parse(const string &body, const vector<Item> &items, Usage &usage) const = 0;

	//! "jev" or "openai". Throws for any other name.
	static unique_ptr<Provider> Create(const string &name);
};

unique_ptr<Provider> CreateJevProvider();
unique_ptr<Provider> CreateOpenAIProvider();

} // namespace typed_ai
} // namespace duckdb
