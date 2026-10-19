#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace orcid {

//! Logical type of an extracted column. The core yields text; the binding
//! layer converts with ParseInteger / ParseBoolean.
enum class ColType { Varchar, Integer, Bigint, Boolean };

//! How a path column turns the matching elements of one item into a value.
enum class ColMode {
	First, //!< the first non-empty value (default)
	Join,  //!< every non-empty value in document order, joined with JOIN_SEPARATOR ('*' prefix)
	Count  //!< number of matching elements, 0 when none ('#' prefix)
};

//! Separator of ColMode::Join values.
constexpr const char *JOIN_SEPARATOR = "; ";

//! One alternative of a column path: an element path relative to the item
//! element (local names, empty = the item itself) and an optional attribute.
//! With `root`, the path is relative to the document element instead ('^'
//! prefix): a document-level value repeated on every row of that document.
struct PathAlt {
	std::vector<std::string> elements;
	std::string attr;
	bool root = false;
};

struct ColumnSpec {
	std::string name;
	ColType type;
	//! "$orcid", "$message_orcid", "$message_primary_orcid", "$hash", "$item", "$kind", "$ordinal", "$position", "$container",
	//! "$group", "$owner", "$owner_put_code", "$doi", "$at", "$event"; empty for
	//! path columns.
	std::string special;
	//! Path alternatives; the first one that yields a non-empty value wins.
	std::vector<PathAlt> alts;
	ColMode mode = ColMode::First;
};

//! Where an item element is recognised: the stack of local names must end with
//! `suffix`; with `root_only` ('^' prefix), the whole stack must equal `suffix`
//! (the pattern is anchored at the document element).
struct ItemPattern {
	bool root_only = false;
	std::vector<std::string> suffix;
};

struct SectionSpec {
	std::string name;
	std::vector<ItemPattern> items;
	std::vector<ColumnSpec> columns;
};

//! Every section, in registration order. Defined by the tables in sections.cpp.
const std::vector<SectionSpec> &Sections();
const SectionSpec *FindSection(const std::string &name);

//! One extracted row: raw trimmed text per column, nullopt for NULL.
using Row = std::vector<std::optional<std::string>>;

//! Appends one row per item of `spec` found in one ORCID v3.0 XML document.
//! Throws XmlError on malformed XML.
void ExtractSection(const SectionSpec &spec, const char *xml, size_t len, std::vector<Row> &out);

//! Format of a document, told by its first byte that is not whitespace (after
//! an optional UTF-8 byte order mark): '{' or '[' is JSON, anything else XML.
enum class DocFormat { Xml, Json };
DocFormat DetectFormat(const char *data, size_t len);

//! ExtractSection for either format. JSON is parsed (JsonError when
//! malformed) and mapped onto the element tree of the equivalent XML document
//! (json_map.hpp), so both formats yield the same rows. `json_root` names the
//! document element of a JSON document, "" infers it (JsonRootError when it
//! cannot be told). `$hash` is the SHA-256 of the `len` bytes given, in
//! either format.
void ExtractDocument(const SectionSpec &spec, const char *data, size_t len, DocFormat format,
                     const std::string &json_root, std::vector<Row> &out);

//! ASCII digits only, 1..max_digits of them; anything else is nullopt.
std::optional<int64_t> ParseInteger(const std::string &text, size_t max_digits);
//! "true"/"1" and "false"/"0"; anything else is nullopt.
std::optional<bool> ParseBoolean(const std::string &text);

} // namespace orcid
