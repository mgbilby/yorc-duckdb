#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace orcid {

class XmlError : public std::runtime_error {
public:
	using std::runtime_error::runtime_error;
};

enum class XmlEvent { StartElement, EndElement, Text, End };

// Minimal non-validating XML pull parser over an in-memory buffer.
// Covers what ORCID v3.0 records use: elements, attributes, text, CDATA, the
// five predefined entities and numeric character references. Comments,
// processing instructions and DOCTYPE are skipped. Namespaces are not
// resolved; use LocalName() (the part after the prefix).
// Self-closing tags produce StartElement followed by EndElement.
class XmlReader {
public:
	XmlReader(const char *data, size_t len) : p_(data), end_(data + len) {
	}

	XmlEvent Next();

	// Valid after StartElement / EndElement.
	const std::string &Name() const {
		return name_;
	}
	const char *LocalName() const;

	// Valid after StartElement.
	bool Attribute(const char *qname, std::string &out) const;

	// Valid after Text (entities decoded; CDATA passed through verbatim).
	const std::string &Text() const {
		return text_;
	}

private:
	void ParseTag();
	void SkipPast(const char *terminator);
	static void DecodeInto(const char *begin, const char *end, std::string &out);

	const char *p_;
	const char *end_;
	std::string name_;
	std::string text_;
	std::vector<std::pair<std::string, std::string>> attrs_;
	bool pending_end_ = false;
};

//! Appends `text` escaped for XML 1.0: as character data, or (attribute =
//! true) as a double-quoted attribute value. '&', '<' and '>' are always
//! escaped, '"' in attributes; CR becomes &#13; (and TAB / LF &#9; / &#10; in
//! attributes) so it survives end-of-line and attribute-value normalization.
//! Throws XmlError if `text` is not valid UTF-8 or holds a character XML 1.0
//! cannot carry at all (U+0000-U+0008, U+000B, U+000C, U+000E-U+001F,
//! U+FFFE, U+FFFF), so invalid input never produces a document.
void AppendXmlEscaped(const std::string &text, bool attribute, std::string &out);

} // namespace orcid
