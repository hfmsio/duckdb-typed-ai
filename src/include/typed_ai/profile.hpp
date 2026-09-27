#pragma once

#include "typed_ai/provider.hpp"

namespace duckdb {
class ExtensionLoader;

namespace typed_ai {

//! Finds the profile to use and fills in provider defaults. Throws a message with the fix when it cannot.
//! `name` empty: the only typed_ai secret, or the built-in jev profile when there is none.
Profile ResolveProfile(ClientContext &context, const string &name, unique_ptr<Provider> &provider);

//! Registers the typed_ai secret type with one provider per backend: CREATE SECRET (TYPE typed_ai, PROVIDER 'jev',
//! ...).
void RegisterSecretType(ExtensionLoader &loader);

} // namespace typed_ai
} // namespace duckdb
