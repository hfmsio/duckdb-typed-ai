#pragma once

#include "duckdb.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"

// One source for DuckDB 1.5 and 2.0. DuckDB 2.0 names things with Identifier where 1.5 used string, and hides
// some fields behind accessors. These helpers cover the difference so the rest of the code reads the same.
// TYPED_AI_DUCKDB_V2 comes from CMakeLists.txt, set from DuckDB's own major version.
#ifdef TYPED_AI_DUCKDB_V2
#include "duckdb/common/identifier.hpp"
#endif

namespace duckdb {
namespace typed_ai {

#ifdef TYPED_AI_DUCKDB_V2
using NameList = vector<Identifier>;

inline Identifier Id(const string &name) {
	return Identifier(name);
}
inline const string &Str(const Identifier &name) {
	return name.GetIdentifierName();
}
inline FunctionData &BindData(ExpressionState &state) {
	return *state.expr.Cast<BoundFunctionExpression>().BindInfo();
}
inline const LogicalType &ExprType(const Expression &expr) {
	return expr.GetReturnType();
}
//! The argument types of a function while it is being bound (a BoundScalarFunction in 2.0).
template <class FUNCTION>
inline vector<LogicalType> &Arguments(FUNCTION &function) {
	return function.GetArguments();
}
template <class T>
inline T *MutableData(Vector &vector) {
	return FlatVector::GetDataMutable<T>(vector);
}
#else
using NameList = vector<string>;

inline const string &Id(const string &name) {
	return name;
}
inline FunctionData &BindData(ExpressionState &state) {
	return *state.expr.Cast<BoundFunctionExpression>().bind_info;
}
inline const LogicalType &ExprType(const Expression &expr) {
	return expr.return_type;
}
inline vector<LogicalType> &Arguments(ScalarFunction &function) {
	return function.arguments;
}
template <class T>
inline T *MutableData(Vector &vector) {
	return FlatVector::GetData<T>(vector);
}
#endif

inline const string &Str(const string &name) {
	return name;
}

} // namespace typed_ai
} // namespace duckdb
