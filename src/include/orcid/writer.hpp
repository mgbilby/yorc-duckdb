#pragma once

#include <cstddef>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace orcid {

//! A record that cannot be written as a valid ORCID v3.0 document: a required
//! field is missing, a value is out of the schema's range, the text is not
//! valid UTF-8, or a bulk document holds too many works. The message names the
//! writer and the field.
class WriteError : public std::runtime_error {
public:
	using std::runtime_error::runtime_error;
};

//! One record to write: its scalar fields as text and its nested lists
//! (LIST(STRUCT) fields in SQL). NULL fields are left out of `fields`, and
//! blank text counts as absent. A list that is not in `lists` was not given
//! (or was NULL), which differs from an empty list for the works' `doi`.
struct WriteRecord {
	std::map<std::string, std::string> fields;
	std::map<std::string, std::vector<WriteRecord>> lists;

	//! The field's text, or nullptr when absent or blank.
	const std::string *Field(const std::string &name) const;
	//! The list, or nullptr when not given.
	const std::vector<WriteRecord> *List(const std::string &name) const;
};

enum class WriteFormat { Xml, Json };

//! The bytes a writer emits for the same document (docs/REFERENCE.md,
//! "Crosswalk mode"). Both write the same elements and values and make the same
//! checks; only the layout differs.
enum class WriteStyle {
	//! yorc-duckdb's own layout (the default): XML indented with tabs, JSON
	//! members in schema order, DEL escaped as \u007f.
	Native,
	//! Exactly what yorc-rs's xml::to_string / json::to_string
	//! write for the same item document (without ORCID's read-only fields) (and yorc-zig, which matches it byte for
	//! byte): XML indented with two spaces; JSON keys sorted, empty always-present
	//! containers as empty arrays, serde_json's string escaping.
	Crosswalk,
};

//! What a writer accepts, for validating its input before writing: scalar
//! fields are the columns of the reader `section` (so a row read with that
//! section can be passed whole; read-only and derived columns are accepted and
//! ignored), plus the nested list fields, each with the shape of its items.
struct RecordShape {
	std::string section;
	std::vector<std::pair<std::string, const RecordShape *>> lists;

	bool AcceptsField(const std::string &name) const;
	//! Shape of the items of list field `name`, or nullptr when it is no list field.
	const RecordShape *ListShape(const std::string &name) const;
};

//! The writers, in registration order: work, funding, affiliation,
//! peer_review, research_resource, person_item, group_id.
const std::vector<std::string> &WriterNames();
//! Input shape of a writer; nullptr for an unknown name.
const RecordShape *WriterShape(const std::string &writer);

//! Serializes `record` as the ORCID v3.0 document of that item
//! (docs/REFERENCE.md, "Writing ORCID documents"): XML with namespaces and
//! the element order of the vendored XSDs, or the JSON of ORCID's own
//! serialization (value wrappers, put-code as a number). Read-only fields
//! (source, created / last-modified dates, path, display index, normalized
//! external ids) are not written; put-code and visibility are. A research
//! resource writes every host of its `hosts` lists (else the flat first host).
//! Throws WriteError.
std::string WriteDocument(const std::string &writer, const WriteRecord &record, WriteFormat format);
//! WriteDocument in the given style (WriteStyle::Native is the overload above).
std::string WriteDocument(const std::string &writer, const WriteRecord &record, WriteFormat format,
                          WriteStyle style);

//! Most works in one bulk document (ORCID's bulk limit).
constexpr size_t MAX_BULK_WORKS = 100;

//! Most hosts of a proposal or resource item (research-resource-3.0.xsd,
//! `hosts`: common:organization maxOccurs 10).
constexpr size_t MAX_RESEARCH_RESOURCE_HOSTS = 10;

//! A proposal or resource item given both a `hosts` list and flat host_*
//! fields: the flat fields must be what the readers derive from the list (each
//! the first non-empty value of that field among the hosts, as
//! orcid_research_resources fills its first-host columns). Returns the first
//! flat field that is not, or "" when they agree or no list is given. The
//! research_resource writer refuses such a record.
std::string HostsConflict(const WriteRecord &record);

//! A bulk document (bulk-3.0.xsd; {"bulk": [{"work": ...}, ...]} in JSON) of
//! 1 to MAX_BULK_WORKS works, each written as WriteDocument("work", ...) does.
std::string WriteBulkWorks(const std::vector<WriteRecord> &works, WriteFormat format);
//! WriteBulkWorks in the given style.
std::string WriteBulkWorks(const std::vector<WriteRecord> &works, WriteFormat format, WriteStyle style);

} // namespace orcid
