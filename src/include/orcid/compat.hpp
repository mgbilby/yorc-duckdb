#pragma once

//! Shims over the DuckDB C++ API surfaces that differ between v1.5.x and v2.0.
//! Every version conditional in the extension lives here; the binding code is
//! written once against these helpers. See docs/DUCKDB2.md.

// Detect v2.0 by a header only v2.0 has (and that the v2.0 code below needs).
// Not identifier.hpp: v1.5.6 backported it while keeping the v1.5 APIs.
#if defined(DUCKDB_MAJOR_VERSION) && DUCKDB_MAJOR_VERSION >= 2
#define ORCID_DUCKDB_V2 1
#elif __has_include("duckdb/common/multi_file/table_function_multi_file.hpp")
#define ORCID_DUCKDB_V2 1
#else
#define ORCID_DUCKDB_V2 0
#endif

#include <type_traits>

#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#if ORCID_DUCKDB_V2
#include "duckdb/common/multi_file/table_function_multi_file.hpp"
#include "duckdb/common/optional.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#endif

namespace duckdb {

// ---------------------------------------------------------------------------
// Column names in table function bind
// ---------------------------------------------------------------------------

//! v2.0 bind callbacks receive vector<Identifier> instead of vector<string>.
//! Deduced from the callback typedef so it tracks whichever DuckDB is in use.
template <class T>
struct OrcidFnArg;
template <class R, class A, class B, class C, class D>
struct OrcidFnArg<R (*)(A, B, C, D)> {
	using Third = C;
	using Fourth = D;
};
using OrcidBindNames = std::remove_reference<OrcidFnArg<table_function_bind_t>::Fourth>::type;

//! A function name built at runtime. v2.0 constructors take an Identifier,
//! which converts implicitly only from string literals.
#if ORCID_DUCKDB_V2
inline Identifier OrcidName(const string &name) {
	return Identifier(name);
}
#else
inline string OrcidName(const string &name) {
	return name;
}
#endif

//! Plain string from a name that is std::string on v1.5.x and Identifier on
//! v2.0 (named-parameter keys, bind names).
template <class T>
inline string OrcidNameString(const T &name) {
	return string(name.c_str(), name.size());
}

// ---------------------------------------------------------------------------
// Vector data access
// ---------------------------------------------------------------------------

//! Writable access to a flat vector's data. v2.0 made FlatVector::GetData
//! const and moved the writable overload to GetDataMutable.
template <class T>
inline T *OrcidFlatData(Vector &vec) {
#if ORCID_DUCKDB_V2
	return FlatVector::GetDataMutable<T>(vec);
#else
	return FlatVector::GetData<T>(vec);
#endif
}

//! Finish an output chunk whose flat vectors were written by index.
//! v2.0 deprecates DataChunk::SetCardinality: vectors carry their own size, and
//! SetChildCardinality sets it on each flat vector without touching the data.
inline void OrcidSetCardinality(DataChunk &chunk, idx_t count) {
#if ORCID_DUCKDB_V2
	chunk.SetChildCardinality(count);
#else
	chunk.SetCardinality(count);
#endif
}

// ---------------------------------------------------------------------------
// Unary execution that can yield SQL NULL
// ---------------------------------------------------------------------------

//! v1.x: UnaryExecutor::ExecuteWithNulls, callback invalidates the row itself.
//! v2.0: that is gone; the callback returns optional<RESULT_TYPE>.
//! `fn(input, is_null)` sets is_null to produce NULL.
template <class INPUT_TYPE, class RESULT_TYPE, class FUNC>
inline void OrcidUnaryWithNulls(Vector &input, Vector &result, idx_t count, FUNC fn) {
#if ORCID_DUCKDB_V2
	UnaryExecutor::Execute<INPUT_TYPE, RESULT_TYPE>(input, result, count,
	                                                [&](INPUT_TYPE in) -> optional<RESULT_TYPE> {
		                                                bool is_null = false;
		                                                auto value = fn(in, is_null);
		                                                if (is_null) {
			                                                return nullopt;
		                                                }
		                                                return value;
	                                                });
#else
	UnaryExecutor::ExecuteWithNulls<INPUT_TYPE, RESULT_TYPE>(
	    input, result, count, [&](INPUT_TYPE in, ValidityMask &mask, idx_t idx) -> RESULT_TYPE {
		    bool is_null = false;
		    auto value = fn(in, is_null);
		    if (is_null) {
			    mask.SetInvalid(idx);
		    }
		    return value;
	    });
#endif
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

//! Declare a named (keyword) parameter of a table function. v2.0 replaced the
//! `named_parameters` map with typed keyword arguments ("**kwargs") on the signature.
inline void OrcidAddNamedParameter(TableFunction &function, const char *name, const LogicalType &type) {
#if ORCID_DUCKDB_V2
	// Typed "**kwargs": that is what the multi-file wrapper forwards to the
	// single-file function it wraps.
	auto &signature = function.GetSignature();
	auto add = [&](TypedKwargs &options) { options.Add(name, type); };
	if (signature.GetTypedKwargs()) {
		signature.ExtendTypedKwargs(add);
	} else {
		signature.WithTypedKwargs("options", add);
	}
#else
	function.named_parameters[name] = type;
#endif
}

//! Register a scalar. v2.0 requires scalars that can throw to declare it, or a
//! thrown error surfaces as an INTERNAL error; v1.5.x has no such flag.
inline void OrcidSetFallible(ScalarFunction &function, bool fallible) {
#if ORCID_DUCKDB_V2
	if (fallible) {
		function.SetFallible();
	}
#else
	(void)function;
	(void)fallible;
#endif
}

inline void OrcidRegisterScalar(ExtensionLoader &loader, ScalarFunction function, bool fallible) {
	OrcidSetFallible(function, fallible);
	loader.RegisterFunction(std::move(function));
}

//! Register overloads of one scalar under `name` (e.g. with and without an
//! optional argument), each marked fallible as OrcidRegisterScalar does.
inline void OrcidRegisterScalarSet(ExtensionLoader &loader, const string &name, vector<ScalarFunction> overloads,
                                   bool fallible) {
	ScalarFunctionSet set(OrcidName(name));
	for (auto &function : overloads) {
		OrcidSetFallible(function, fallible);
		set.AddFunction(std::move(function));
	}
	loader.RegisterFunction(std::move(set));
}

// ---------------------------------------------------------------------------
// Settings (SET name = value)
// ---------------------------------------------------------------------------

//! Register a BOOLEAN extension setting (`SET orcid_crosswalk = true`), with
//! its default. v1.5.6: DBConfig::AddExtensionOption on the loader's database.
//! Not yet built against v2.0 (docs/DUCKDB2.md); if v2.0 renames either call,
//! the shim goes here.
inline void OrcidAddBooleanSetting(ExtensionLoader &loader, const string &name, const string &description,
                                   bool default_value) {
	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
#if ORCID_DUCKDB_V2
	config.AddExtensionOption(Identifier(name), description, LogicalType::BOOLEAN, Value::BOOLEAN(default_value));
#else
	config.AddExtensionOption(name, description, LogicalType::BOOLEAN, Value::BOOLEAN(default_value));
#endif
}

//! The current value of a BOOLEAN setting registered above: false when it is
//! unset, NULL or unknown.
inline bool OrcidBooleanSetting(ClientContext &context, const string &name) {
	Value value;
#if ORCID_DUCKDB_V2
	if (!context.TryGetCurrentSetting(Identifier(name), value) || value.IsNull()) {
#else
	if (!context.TryGetCurrentSetting(name, value) || value.IsNull()) {
#endif
		return false;
	}
	return BooleanValue::Get(value.DefaultCastAs(LogicalType::BOOLEAN));
}

//! The same table function with one more positional VARCHAR argument (the
//! json_root lateral overloads of the _json twins and orcid_changes_json).
//! Call this before OrcidAddNamedParameter: a v2.0 signature must order every
//! positional parameter before the "**options" of the typed keyword
//! arguments, so the extra argument is appended to a copy (a function object
//! carries its signature by value) before the kwargs go on. v1.5.6:
//! TableFunction::arguments is a vector, and named parameters are a map, so
//! the order never mattered there.
inline TableFunction OrcidWithVarcharArgument(TableFunction function) {
#if ORCID_DUCKDB_V2
	function.GetSignature().AddParameter(LogicalType::VARCHAR);
#else
	function.arguments.push_back(LogicalType::VARCHAR);
#endif
	return function;
}

//! Register a file reader that takes one VARCHAR path.
//!
//! v2.0: registered as a single-file function inside the multi-file framework,
//! which then supplies globbing, the LIST(VARCHAR) overload, union_by_name,
//! hive partitioning, the filename/file_index virtual columns, and scheduling
//! of files across threads.
//! v1.5.x: registered as-is; the reader expands globs itself
//! (OrcidReaderGlobsItself) and parallelizes over files and archive members.
inline void OrcidRegisterReader(ExtensionLoader &loader, TableFunction function, const string &name,
                                const char *reader_type) {
#if ORCID_DUCKDB_V2
	TableFunctionMultiFileSettings settings;
	settings.reader_type = reader_type;
	// Fixed column list: one file determines the schema.
	settings.maximum_sample_files = 1;
	loader.RegisterFunction(
	    TableFunctionMultiFileWrapper::CreateFunctionSet(std::move(function), Identifier(name), std::move(settings)));
#else
	(void)name;
	(void)reader_type;
	loader.RegisterFunction(std::move(function));
#endif
}

//! True only on v1.5.x. Under the v2.0 multi-file framework the reader is
//! handed one concrete file and must not glob again.
inline constexpr bool OrcidReaderGlobsItself() {
	return !ORCID_DUCKDB_V2;
}

} // namespace duckdb
