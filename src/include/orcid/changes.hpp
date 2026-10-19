#pragma once

#include "orcid/sections.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace orcid {

//! One difference between two snapshots of an ORCID record.
struct ChangeRow {
	std::string orcid;
	std::string section; // works, fundings, affiliations, ...
	std::string kind;    // item kind where the section has several (employment, keyword, ...)
	std::optional<std::string> put_code;
	std::string change;  // "added", "removed" or "modified"
	std::optional<std::string> field; // changed column, for "modified"
	std::optional<std::string> old_value;
	std::optional<std::string> new_value;
	std::optional<std::string> label; // title / role / value of the item (new side preferred)
};

//! Compares two ORCID v3.0 documents (any shape the sections accept) and
//! appends their differences, section by section, in document order: modified
//! and added items as they appear in `new_xml`, then removed items as they
//! appeared in `old_xml`. Items are matched on (section, kind, put-code), or on
//! the section alone for single-item sections (person, history). Every column
//! is compared except `source_hash` and `item`. Throws XmlError on malformed XML.
void DiffDocuments(const char *old_xml, size_t old_len, const char *new_xml, size_t new_len,
                   std::vector<ChangeRow> &out);

//! The same for documents in either format (XML or JSON, see ExtractDocument);
//! the two sides need not share a format. `json_root` applies to JSON sides.
//! Throws XmlError / JsonError / JsonRootError.
void DiffDocuments(const char *old_doc, size_t old_len, DocFormat old_format, const char *new_doc, size_t new_len,
                   DocFormat new_format, const std::string &json_root, std::vector<ChangeRow> &out);

//! Sections DiffDocuments compares, in output order.
const std::vector<std::string> &DiffSections();

} // namespace orcid
