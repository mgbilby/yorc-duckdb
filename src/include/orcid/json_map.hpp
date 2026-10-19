#pragma once

#include "orcid/json.hpp"
#include "orcid/xml.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace orcid {

//! A JSON document whose document element cannot be told from its content
//! (ORCID's JSON has no root name): an affiliation without a type, or an
//! other name / keyword. The message names the json_root values that fit.
class JsonRootError : public std::runtime_error {
public:
	using std::runtime_error::runtime_error;
};

//! One element of the ORCID v3.0 XML model: local name (no namespaces),
//! attributes, text (leaf elements) and child elements in document order.
struct Element {
	std::string name;
	std::vector<std::pair<std::string, std::string>> attrs;
	std::string text;
	std::vector<Element> children;
};

//! Maps one parsed ORCID v3.0 JSON document (Jackson's serialization of the
//! same model as the XML) onto the element tree of the equivalent XML
//! document, so that the table-driven section extractor reads both formats
//! alike. One root element per top-level object; a top-level array is a
//! sequence of documents (each object one root; other values are skipped).
//! `root` names the document element; empty infers it (InferJsonRoot).
//!
//! Mapping (docs/REFERENCE.md, "JSON documents"):
//! - a member is a child element of the same name; null members are absent;
//!   an array is the element repeated, once per non-null entry;
//! - {"value": v, ...} with otherwise only language-code, currency-code,
//!   transient or visibility members is a leaf element with text v and those
//!   attributes (title, url, source-name, dates, amount, ...);
//! - scalar put-code, path, visibility, display-index, verified, primary,
//!   num-found, transient and lang (xml:lang) are attributes; put-code is
//!   also kept as a child element (record-summary items carry it as one);
//!   "path" is a child element in identifier objects ({uri, path, host});
//! - "summaries" (affiliation-group) and "bulk" hold arrays of single-member
//!   wrapper objects; the wrapped elements become children of the parent;
//! - "resource-item" outside a "resource-items" element is placed inside one
//!   (research-resource JSON drops that wrapper);
//! - an integer "value" of a member named *-date, or such a member that is a
//!   bare integer, is epoch milliseconds and becomes the ISO 8601 UTC form
//!   ORCID's XML uses (EpochMillisToIso);
//! - other scalars are text: strings as decoded, numbers as written,
//!   booleans as "true" / "false".
//! - history/creation-method: the Java enum names (API, DIRECT,
//!   MEMBER_REFERRED, WEBSITE) become the XSD values (API, Direct,
//!   Member-referred, website); other text is kept (as yorc-rs reads it);
//! - the children of record, person, personal-details, record-summary and
//!   research-resource are put in XSD sequence order (kChildOrders), whatever
//!   the key order; children of one name keep their order.
void JsonToElements(const JsonValue &doc, const std::string &root, std::vector<Element> &out);

//! The document element of a top-level JSON object: from a "bulk" member,
//! else from its "path" ("/<iD>/work/123" -> work, "/<iD>" -> record), else
//! from the members it has. Empty when the shape is not an ORCID document.
//! Throws JsonRootError when it is one but ambiguous (see JsonRootError).
std::string InferJsonRoot(const JsonValue &object);

//! Epoch milliseconds (ORCID's JSON timestamps) in the form ORCID's XML
//! writes them: UTC, millisecond precision, "Z" (2018-10-30T10:49:18.250Z).
std::string EpochMillisToIso(int64_t ms);

//! Replays an element tree as XmlReader events, with the same accessors, so
//! one extractor serves both formats. Each element yields StartElement, then
//! Text when its text is not empty, then its children, then EndElement.
class ElementReader {
public:
	explicit ElementReader(const Element &root) : root_(&root) {
	}

	XmlEvent Next();

	const std::string &Name() const {
		return current_ ? current_->name : empty_;
	}
	const char *LocalName() const {
		return Name().c_str();
	}
	bool Attribute(const char *qname, std::string &out) const;
	const std::string &Text() const {
		return current_ ? current_->text : empty_;
	}

private:
	struct Position {
		const Element *element;
		size_t next_child;
		bool text_done;
	};
	const Element *root_;
	const Element *current_ = nullptr;
	std::vector<Position> stack_;
	bool started_ = false;
	std::string empty_;
};

} // namespace orcid
