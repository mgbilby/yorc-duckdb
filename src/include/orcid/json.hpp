#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace orcid {

class JsonError : public std::runtime_error {
public:
	using std::runtime_error::runtime_error;
};

//! One parsed JSON value (RFC 8259). Object members keep document order and
//! duplicate keys; numbers keep their literal text, so nothing is lost to
//! floating point.
struct JsonValue {
	enum class Type { Null, Bool, Number, String, Array, Object };
	Type type = Type::Null;
	//! String: the decoded UTF-8 text; Number: the literal as written; Bool:
	//! "true" or "false"; empty otherwise.
	std::string text;
	std::vector<JsonValue> items;                           //!< Array
	std::vector<std::pair<std::string, JsonValue>> members; //!< Object

	//! First member named `key`, or nullptr (always nullptr for a non-object).
	const JsonValue *Find(const std::string &key) const;
	//! True when the object has a member `key`, even a null one.
	bool Has(const std::string &key) const {
		return Find(key) != nullptr;
	}
	bool IsScalar() const {
		return type == Type::Bool || type == Type::Number || type == Type::String;
	}
};

//! Deepest nesting of arrays and objects ParseJson accepts (ORCID documents
//! nest about 12 deep). Bounds the parser's recursion.
constexpr size_t JSON_MAX_DEPTH = 256;

//! Parses one JSON text: an optional UTF-8 byte order mark, whitespace, one
//! value, whitespace. Strict RFC 8259: no comments, trailing commas, NaN or
//! single quotes; control characters in strings must be escaped; strings must
//! be valid UTF-8 and \u escapes must pair surrogates. Throws JsonError naming
//! the byte offset.
JsonValue ParseJson(const char *data, size_t len);

//! True when [data, data + len) is well-formed UTF-8 (RFC 3629: shortest
//! forms only, no surrogates, nothing above U+10FFFF).
bool IsValidUtf8(const char *data, size_t len);

//! Length of the longest well-formed UTF-8 prefix of [data, data + len): `len`
//! when all of it is valid, else the byte offset of the first malformed
//! sequence. One pass; runs of ASCII are skipped eight bytes at a time.
size_t Utf8ValidPrefix(const char *data, size_t len);

//! Decodes the UTF-8 sequence at `p` (before `end`) into `cp` and advances
//! `p`. Returns false, leaving `p` alone, on a malformed sequence.
bool DecodeUtf8(const char *&p, const char *end, uint32_t &cp);

//! Appends code point `cp` (at most U+10FFFF, not a surrogate) as UTF-8.
void AppendCodePoint(uint32_t cp, std::string &out);

//! Appends `text` as a JSON string literal, quotes included. '"', '\\' and
//! control characters are escaped (\b \f \n \r \t, other C0 characters and
//! DEL as \u00XX), and U+2028 / U+2029 as   /   so the output is
//! also a valid JavaScript literal; everything else, non-BMP characters
//! included, is copied as UTF-8. Throws JsonError if `text` is not valid
//! UTF-8, so invalid input never produces a document.
void AppendJsonString(const std::string &text, std::string &out);

} // namespace orcid
