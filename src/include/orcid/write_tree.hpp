#pragma once

//! The rendered item the writers serialize, shared by the default emitter
//! (writer.cpp) and the crosswalk emitter (crosswalk.cpp). Internal to the
//! core: callers use orcid/writer.hpp.
//!
//! A writer turns its record into this tree once: the template of the item
//! (writer.cpp, from the XSDs), the derived fields (Prepare) and every check
//! the writer makes. Both emitters then only lay it out, so they write the same
//! elements, values and errors and differ in bytes alone.

#include "orcid/writer.hpp"

#include <string>
#include <vector>

namespace orcid {
namespace write_tree {

//! Template flags (writer.cpp); the emitters read VALUE, ARRAY, FLATTEN and ALWAYS.
enum : unsigned {
	REQUIRED = 1u << 0,  //!< must be written when its parent is (minOccurs 1); a list: at least one item
	ALWAYS = 1u << 1,    //!< written even with no content (required containers whose children are optional)
	VALUE = 1u << 2,     //!< JSON: {"value": text, <attributes>} instead of a string
	ARRAY = 1u << 3,     //!< JSON: a one-element array (research-resource hosts/organization)
	FLATTEN = 1u << 4,   //!< JSON: no member of its own, its children join the parent (resource-items)
	YEAR = 1u << 5,      //!< text: common:year, an integer 1900..2100
	MONTH_DAY = 1u << 6, //!< text: common:month (1..12) / common:day (1..31), written with two digits
	INTEGER = 1u << 7,   //!< text: xs:integer (put-code); a JSON number
};

struct WrittenAttr {
	std::string name;
	std::string value;
	bool integer; //!< an xs:integer (put-code): a JSON number
};

//! One element of the document to write.
struct Written {
	std::string ns;
	std::string name;
	std::vector<WrittenAttr> attrs;
	std::string text;
	bool leaf = false;
	bool repeated = false; //!< one of a list's elements (a JSON array)
	unsigned flags = 0;
	std::vector<Written> children;
	//! Element names of list children (one element per list item) that wrote
	//! no element here. The default emitter ignores them; the crosswalk emitter
	//! writes them as empty JSON arrays, as yorc-rs writes an always-present
	//! container (`"external-ids": {"external-id": []}`).
	std::vector<std::string> empty_lists;
};

//! The document element of `writer` for `record`, with every check of
//! WriteDocument. Throws WriteError.
Written RenderDocument(const std::string &writer, const WriteRecord &record);

//! bulk:bulk holding 1 to MAX_BULK_WORKS works, with every check of
//! WriteBulkWorks. Throws WriteError.
Written RenderBulkWorks(const std::vector<WriteRecord> &works);

//! The crosswalk emitter (crosswalk.cpp): `root` as yorc-rs writes the
//! document (xml::to_string / json::to_string). A root named
//! bulk:bulk is written as yorc-rs's bulk (`{"bulk": [{"work": ...}]}` in
//! JSON). Throws XmlError / JsonError (characters XML cannot carry, invalid
//! UTF-8) and WriteError (a put-code outside yorc-rs's i64).
std::string CrosswalkXml(const Written &root);
std::string CrosswalkJson(const Written &root);

//! A put-code as yorc-rs prints it after reading it (an i64 in decimal:
//! no sign for positive numbers, no leading zeros). `text` is what the writer
//! accepted: optional '-' and digits. Returns false when it does not fit an i64.
bool CrosswalkInteger(const std::string &text, std::string &out);

//! Appends `text` as serde_json writes a string (yorc-rs's JSON): quotes,
//! '"' and '\\' escaped, \b \f \n \r \t, the other C0 controls as \u00xx (lower-case
//! hex), and everything else, DEL and U+2028 / U+2029 included, as UTF-8.
//! Throws JsonError if `text` is not valid UTF-8.
void AppendSerdeJsonString(const std::string &text, std::string &out);

} // namespace write_tree
} // namespace orcid
