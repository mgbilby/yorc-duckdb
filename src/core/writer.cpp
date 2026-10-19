#include "orcid/writer.hpp"

#include "orcid/json.hpp"
#include "orcid/write_tree.hpp"
#include "orcid/sections.hpp"
#include "orcid/xml.hpp"

#include <set>

namespace orcid {

const std::string *WriteRecord::Field(const std::string &name) const {
	auto it = fields.find(name);
	if (it == fields.end()) {
		return nullptr;
	}
	for (char c : it->second) {
		if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
			return &it->second;
		}
	}
	return nullptr;
}

const std::vector<WriteRecord> *WriteRecord::List(const std::string &name) const {
	auto it = lists.find(name);
	return it == lists.end() ? nullptr : &it->second;
}

bool RecordShape::AcceptsField(const std::string &name) const {
	const SectionSpec *spec = FindSection(section);
	if (!spec) {
		return false;
	}
	for (auto &col : spec->columns) {
		if (col.name == name) {
			return true;
		}
	}
	return false;
}

const RecordShape *RecordShape::ListShape(const std::string &name) const {
	for (auto &l : lists) {
		if (l.first == name) {
			return l.second;
		}
	}
	return nullptr;
}

namespace {

// ---------------------------------------------------------------------------
// Templates
//
// One template per writable ORCID item, transcribed from the vendored XSDs
// (record_3.0/*-3.0.xsd, common_3.0, group-id-3.0): element names, namespace
// prefixes and the order of each xs:sequence. Leaves name the record field
// that holds their text; the field names are the reader columns, so a row read
// by orcid_<section> writes back to the same document. JSON forms follow
// ORCID's own serialization (read_samples/record-full-3.0.json, the
// write_samples): VALUE leaves are {"value": ...} objects there.
// ---------------------------------------------------------------------------

// The template flags (REQUIRED, ALWAYS, VALUE, ...) are in orcid/write_tree.hpp,
// which the crosswalk emitter shares.
using namespace write_tree;

struct Attr {
	std::string name;
	std::string field;
	unsigned flags;
};

struct Node {
	std::string ns;
	std::string name;
	std::string field; //!< leaf: the record field holding the text
	unsigned flags = 0;
	std::string list;  //!< one element per item of this list field of the record
	std::string needs; //!< leaf: invalid without this field too (a day needs its month)
	std::vector<Attr> attrs;
	std::vector<Node> children;
};

Node Leaf(const std::string &ns, const std::string &name, const std::string &field, unsigned flags = 0,
          std::vector<Attr> attrs = {}) {
	Node n;
	n.ns = ns;
	n.name = name;
	n.field = field;
	n.flags = flags;
	n.attrs = std::move(attrs);
	return n;
}

Node Group(const std::string &ns, const std::string &name, unsigned flags, std::vector<Node> children) {
	Node n;
	n.ns = ns;
	n.name = name;
	n.flags = flags;
	n.children = std::move(children);
	return n;
}

Node Repeat(const std::string &ns, const std::string &name, const std::string &list, unsigned flags,
            std::vector<Node> children) {
	Node n = Group(ns, name, flags, std::move(children));
	n.list = list;
	return n;
}

//! The document element of an item: put-code (needed to update an item) and
//! visibility, as element-summary declares them.
Node Root(const std::string &ns, const std::string &name, std::vector<Node> children) {
	Node n = Group(ns, name, ALWAYS, std::move(children));
	n.attrs = {{"put-code", "put_code", INTEGER}, {"visibility", "visibility", 0}};
	return n;
}

//! common:external-id's sequence; normalized forms are transient (ORCID's) and not written.
std::vector<Node> ExternalIdLeaves(const std::string &type, const std::string &value, const std::string &url,
                                   const std::string &relationship) {
	return {Leaf("common", "external-id-type", type, REQUIRED), Leaf("common", "external-id-value", value, REQUIRED),
	        Leaf("common", "external-id-url", url, VALUE), Leaf("common", "external-id-relationship", relationship)};
}

Node ExternalIds(unsigned flags = 0) {
	return Group("common", "external-ids", flags,
	             {Repeat("common", "external-id", "external_ids", 0,
	                     ExternalIdLeaves("type", "value", "url", "relationship"))});
}

//! common:fuzzy-date from <prefix>year / month / day.
Node FuzzyDate(const std::string &ns, const std::string &name, const std::string &prefix, unsigned flags = 0) {
	Node day = Leaf("common", "day", prefix + "day", VALUE | MONTH_DAY);
	day.needs = prefix + "month";
	return Group(ns, name, flags,
	             {Leaf("common", "year", prefix + "year", REQUIRED | VALUE | YEAR),
	              Leaf("common", "month", prefix + "month", VALUE | MONTH_DAY), day});
}

//! common:organization from <prefix>name / city / region / country / disambiguated_id / disambiguation_source.
Node Organization(const std::string &ns, const std::string &name, const std::string &prefix, unsigned flags = 0) {
	return Group(ns, name, flags,
	             {Leaf("common", "name", prefix + "name", REQUIRED),
	              Group("common", "address", ALWAYS,
	                    {Leaf("common", "city", prefix + "city"), Leaf("common", "region", prefix + "region"),
	                     Leaf("common", "country", prefix + "country")}),
	              Group("common", "disambiguated-organization", 0,
	                    {Leaf("common", "disambiguated-organization-identifier", prefix + "disambiguated_id", REQUIRED),
	                     Leaf("common", "disambiguation-source", prefix + "disambiguation_source", REQUIRED)})});
}

//! common:translated-title: its language-code attribute is required.
Node TranslatedTitle(const std::string &field, const std::string &language_field) {
	return Leaf("common", "translated-title", field, VALUE, {{"language-code", language_field, REQUIRED}});
}

//! common:orcid-id's optional host, after uri and path (the write samples carry
//! it: work-full-3.0.xml). It needs the path the identifier is chosen by.
Node ContributorHost() {
	Node host = Leaf("common", "host", "contributor_orcid_host");
	host.needs = "contributor_orcid";
	return host;
}

//! work:contributors / funding:contributors (funding's attributes have no sequence:
//! Prepare refuses a funding contributor's `sequence`).
Node Contributors(const std::string &ns, bool sequence) {
	std::vector<Node> attributes;
	if (sequence) {
		attributes.push_back(Leaf(ns, "contributor-sequence", "sequence"));
	}
	attributes.push_back(Leaf(ns, "contributor-role", "role"));
	return Group(ns, "contributors", 0,
	             {Repeat(ns, "contributor", "contributors", 0,
	                     {Group("common", "contributor-orcid", 0,
	                            {Leaf("common", "uri", "contributor_orcid_uri"), Leaf("common", "path", "contributor_orcid"),
	                             ContributorHost()}),
	                      Leaf(ns, "credit-name", "credit_name", VALUE), Leaf(ns, "contributor-email", "email"),
	                      Group(ns, "contributor-attributes", 0, std::move(attributes))})});
}

// work-3.0.xsd: work
Node WorkTemplate() {
	return Root("work", "work",
	            {Group("work", "title", REQUIRED,
	                   {Leaf("common", "title", "title", REQUIRED | VALUE), Leaf("common", "subtitle", "subtitle", VALUE),
	                    TranslatedTitle("translated_title", "translated_title_language")}),
	             Leaf("work", "journal-title", "journal", VALUE), Leaf("work", "short-description", "short_description"),
	             Group("work", "citation", 0,
	                   {Leaf("work", "citation-type", "citation_type", REQUIRED),
	                    Leaf("work", "citation-value", "citation_value", REQUIRED)}),
	             Leaf("work", "type", "type", REQUIRED), FuzzyDate("common", "publication-date", "pub_"), ExternalIds(),
	             Leaf("common", "url", "url", VALUE), Contributors("work", true),
	             Leaf("common", "language-code", "language_code"), Leaf("common", "country", "country", VALUE)});
}

// funding-3.0.xsd: funding
Node FundingTemplate() {
	return Root("funding", "funding",
	            {Leaf("funding", "type", "type", REQUIRED),
	             Leaf("funding", "organization-defined-type", "organization_defined_type", VALUE),
	             Group("funding", "title", 0,
	                   {Leaf("common", "title", "title", REQUIRED | VALUE),
	                    TranslatedTitle("translated_title", "translated_title_language")}),
	             Leaf("funding", "short-description", "short_description"),
	             Leaf("funding", "amount", "amount", VALUE, {{"currency-code", "currency", REQUIRED}}),
	             Leaf("common", "url", "url", VALUE), FuzzyDate("common", "start-date", "start_"),
	             FuzzyDate("common", "end-date", "end_"), ExternalIds(), Contributors("funding", false),
	             Organization("common", "organization", "org_", REQUIRED)});
}

// common-3.0.xsd: affiliation (distinction, education, employment, invited-position,
// membership, qualification, service: one type, seven document elements)
Node AffiliationTemplate(const std::string &type) {
	return Root(type, type,
	            {Leaf("common", "department-name", "department"), Leaf("common", "role-title", "role_title"),
	             FuzzyDate("common", "start-date", "start_"), FuzzyDate("common", "end-date", "end_"),
	             Organization("common", "organization", "org_"), Leaf("common", "url", "url", VALUE), ExternalIds()});
}

// peer-review-3.0.xsd: peer-review
Node PeerReviewTemplate() {
	return Root(
	    "peer-review", "peer-review",
	    {Leaf("peer-review", "reviewer-role", "reviewer_role", REQUIRED),
	     Group("peer-review", "review-identifiers", REQUIRED | ALWAYS,
	           {Repeat("common", "external-id", "external_ids", 0,
	                   ExternalIdLeaves("type", "value", "url", "relationship"))}),
	     Leaf("peer-review", "review-url", "review_url", VALUE), Leaf("peer-review", "review-type", "review_type", REQUIRED),
	     FuzzyDate("peer-review", "review-completion-date", "completion_", REQUIRED),
	     Leaf("peer-review", "review-group-id", "review_group_id", REQUIRED),
	     Group("peer-review", "subject-external-identifier", 0,
	           ExternalIdLeaves("subject_id_type", "subject_id_value", "subject_id_url", "subject_id_relationship")),
	     Leaf("peer-review", "subject-container-name", "subject_container_name"),
	     Leaf("peer-review", "subject-type", "subject_type"),
	     Group("peer-review", "subject-name", 0,
	           {Leaf("common", "title", "subject_name", REQUIRED | VALUE),
	            Leaf("common", "subtitle", "subject_subtitle", VALUE),
	            TranslatedTitle("subject_translated_name", "subject_translated_name_language")}),
	     Leaf("peer-review", "subject-url", "subject_url", VALUE),
	     Organization("peer-review", "convening-organization", "convening_org_", REQUIRED)});
}

//! research-resource:hosts (research-resource-3.0.xsd: 1 to 10 common:organization):
//! one organization per item of the `hosts` list field (the columns of
//! orcid_research_resource_hosts) when it is given, else one from the flat host_*
//! fields (the first-host columns). Prepare drops the flat fields when the list
//! is given, after checking that they agree with it.
Node Hosts() {
	Node listed = Organization("common", "organization", "host_");
	listed.list = "hosts";
	return Group("research-resource", "hosts", REQUIRED,
	             {Organization("common", "organization", "host_", ARRAY), std::move(listed)});
}

// research-resource-3.0.xsd: research-resource (every host of the proposal and of
// each item from their `hosts` lists, else the flat first host)
Node ResearchResourceTemplate() {
	return Root(
	    "research-resource", "research-resource",
	    {Group("research-resource", "proposal", REQUIRED,
	           {Group("research-resource", "title", REQUIRED,
	                  {Leaf("common", "title", "title", REQUIRED | VALUE),
	                   TranslatedTitle("translated_title", "translated_title_language")}),
	            Hosts(), ExternalIds(REQUIRED | ALWAYS), FuzzyDate("common", "start-date", "start_"),
	            FuzzyDate("common", "end-date", "end_"), Leaf("common", "url", "url", VALUE)}),
	     Group("research-resource", "resource-items", REQUIRED | ALWAYS | FLATTEN,
	           {Repeat("research-resource", "resource-item", "resource_items", REQUIRED,
	                   {Leaf("research-resource", "resource-name", "resource_name", REQUIRED),
	                    Leaf("research-resource", "resource-type", "resource_type", REQUIRED),
	                    Hosts(), ExternalIds(REQUIRED | ALWAYS), Leaf("common", "url", "url", VALUE)})})});
}

// address-3.0, keyword-3.0, other-name-3.0, researcher-url-3.0,
// person-external-identifier-3.0: person items, by the reader's `kind`.
Node PersonItemTemplate(const std::string &kind) {
	if (kind == "address") {
		return Root("address", "address", {Leaf("address", "country", "value", REQUIRED | VALUE)});
	}
	if (kind == "keyword") {
		return Root("keyword", "keyword", {Leaf("keyword", "content", "value", REQUIRED)});
	}
	if (kind == "other-name") {
		return Root("other-name", "other-name", {Leaf("other-name", "content", "value", REQUIRED)});
	}
	if (kind == "researcher-url") {
		return Root("researcher-url", "researcher-url",
		            {Leaf("researcher-url", "url-name", "value"), Leaf("researcher-url", "url", "url", REQUIRED | VALUE)});
	}
	if (kind == "external-identifier") {
		return Root("external-identifier", "external-identifier",
		            ExternalIdLeaves("type", "value", "url", "relationship"));
	}
	throw WriteError("person_item: kind must be one of address, keyword, other-name, researcher-url, "
	                 "external-identifier (got '" +
	                 kind + "')");
}

// group-id-3.0.xsd: group-id-record (the inline source fields are ORCID's)
Node GroupIdTemplate() {
	Node n = Group("group-id", "group-id-record", ALWAYS,
	               {Leaf("group-id", "name", "name", REQUIRED), Leaf("group-id", "group-id", "group_id", REQUIRED),
	                Leaf("group-id", "description", "description", REQUIRED),
	                Leaf("group-id", "type", "type", REQUIRED)});
	n.attrs = {{"put-code", "put_code", INTEGER}};
	return n;
}

//! The flat first-host fields of research_resources and research_resource_items;
//! the same names are the fields of a `hosts` list item.
const char *const kHostFields[] = {"host_name", "host_city", "host_region", "host_country", "host_disambiguated_id",
                                   "host_disambiguation_source"};

const char *const kAffiliationTypes[] = {"distinction", "education",     "employment", "invited-position",
                                         "membership",  "qualification", "service"};

// ---------------------------------------------------------------------------
// Rendering: template + record -> output tree, checking what the XSDs require
// ---------------------------------------------------------------------------

//! The output tree (orcid/write_tree.hpp): the same one feeds the crosswalk emitter.
using OutAttr = WrittenAttr;
using Out = Written;

bool AllDigits(const std::string &s) {
	if (s.empty()) {
		return false;
	}
	for (char c : s) {
		if (c < '0' || c > '9') {
			return false;
		}
	}
	return true;
}

std::string Trimmed(const std::string &s) {
	size_t b = 0, e = s.size();
	while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) {
		b++;
	}
	while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) {
		e--;
	}
	return s.substr(b, e - b);
}

class Renderer {
public:
	explicit Renderer(std::string writer) : writer_(std::move(writer)) {
	}

	//! The document element; always written.
	Out RenderRoot(const Node &root, const WriteRecord &record, const std::string &where) {
		Out o;
		bool content = false;
		RenderOne(root, record, where, o, content);
		return o;
	}

private:
	[[noreturn]] void Fail(const std::string &message) const {
		throw WriteError(writer_ + ": " + message);
	}

	//! The first record field below `node`, to name what a missing element needs.
	static std::string FirstField(const Node &node) {
		if (!node.field.empty()) {
			return node.field;
		}
		if (!node.list.empty()) {
			return node.list;
		}
		for (auto &c : node.children) {
			std::string f = FirstField(c);
			if (!f.empty()) {
				return f;
			}
		}
		return std::string();
	}

	std::string Format(const std::string &raw, unsigned flags, const std::string &where,
	                   const std::string &field) const {
		if (!(flags & (YEAR | MONTH_DAY | INTEGER))) {
			return raw;
		}
		std::string v = Trimmed(raw);
		if (flags & INTEGER) {
			std::string digits = !v.empty() && v[0] == '-' ? v.substr(1) : v;
			if (!AllDigits(digits)) {
				Fail("'" + where + field + "' must be an integer, got '" + raw + "'");
			}
			return v;
		}
		if (!AllDigits(v) || v.size() > 9) {
			Fail("'" + where + field + "' must be a number, got '" + raw + "'");
		}
		const long n = std::stol(v);
		long lo = 1900, hi = 2100;
		if (flags & MONTH_DAY) {
			const bool month = field.size() >= 5 && field.compare(field.size() - 5, 5, "month") == 0;
			lo = 1;
			hi = month ? 12 : 31;
		}
		if (n < lo || n > hi) {
			Fail("'" + where + field + "' is " + std::to_string(n) + ", outside the schema's range " +
			     std::to_string(lo) + ".." + std::to_string(hi));
		}
		std::string out = std::to_string(n);
		if ((flags & MONTH_DAY) && out.size() < 2) {
			out = "0" + out;
		}
		return out;
	}

	//! Renders `node` (zero or more elements for a list) into `out`. Returns
	//! whether an element was written; `content` is set when one carries data.
	bool Render(const Node &node, const WriteRecord &record, const std::string &where, std::vector<Out> &out,
	            bool &content) {
		if (node.list.empty()) {
			Out o;
			bool c = false;
			if (!RenderOne(node, record, where, o, c)) {
				return false;
			}
			content = content || c;
			out.push_back(std::move(o));
			return true;
		}
		const std::vector<WriteRecord> *items = record.List(node.list);
		size_t written = 0;
		if (items) {
			for (size_t i = 0; i < items->size(); i++) {
				Out o;
				bool c = false;
				const std::string item_where = where + node.list + "[" + std::to_string(i + 1) + "].";
				if (RenderOne(node, (*items)[i], item_where, o, c)) {
					o.repeated = true;
					content = content || c;
					out.push_back(std::move(o));
					written++;
				}
			}
		}
		if (written == 0 && (node.flags & REQUIRED)) {
			Fail("'" + where + node.list + "' needs at least one item");
		}
		return written > 0;
	}

	bool RenderOne(const Node &node, const WriteRecord &record, const std::string &where, Out &o, bool &content) {
		o.ns = node.ns;
		o.name = node.name;
		o.flags = node.flags;
		for (auto &a : node.attrs) {
			const std::string *v = record.Field(a.field);
			if (v) {
				o.attrs.push_back({a.name, Format(*v, a.flags, where, a.field), (a.flags & INTEGER) != 0});
			}
		}
		if (!node.field.empty()) {
			o.leaf = true;
			const std::string *v = record.Field(node.field);
			if (!v) {
				return false;
			}
			for (auto &a : node.attrs) {
				if ((a.flags & REQUIRED) && !record.Field(a.field)) {
					Fail("'" + where + a.field + "' is required with '" + where + node.field + "'");
				}
			}
			if (!node.needs.empty() && !record.Field(node.needs)) {
				Fail("'" + where + node.field + "' needs '" + where + node.needs + "'");
			}
			o.text = Format(*v, node.flags, where, node.field);
			content = true;
			return true;
		}
		bool any = !o.attrs.empty();
		const Node *missing = nullptr;
		for (auto &child : node.children) {
			bool c = false;
			const bool written = Render(child, record, where, o.children, c);
			any = any || c;
			if (!written && !child.list.empty()) {
				o.empty_lists.push_back(child.name); // the crosswalk's empty JSON arrays
			}
			if (!written && (child.flags & REQUIRED) && child.list.empty() && !missing) {
				missing = &child;
			}
		}
		if (!any && !(node.flags & ALWAYS)) {
			o.children.clear();
			return false;
		}
		if (missing) {
			Fail("'" + where + FirstField(*missing) + "' is required (" + missing->ns + ":" + missing->name + ")");
		}
		content = any;
		return true;
	}

	std::string writer_;
};

// ---------------------------------------------------------------------------
// Serialization
// ---------------------------------------------------------------------------

void CollectPrefixes(const Out &o, std::set<std::string> &out) {
	out.insert(o.ns);
	for (auto &c : o.children) {
		CollectPrefixes(c, out);
	}
}

void XmlElement(const Out &o, size_t depth, std::string &out) {
	out.append(depth, '\t');
	out += "<" + o.ns + ":" + o.name;
	if (depth == 0) {
		std::set<std::string> prefixes;
		CollectPrefixes(o, prefixes);
		for (auto &p : prefixes) {
			out += " xmlns:" + p + "=\"http://www.orcid.org/ns/" + p + "\"";
		}
	}
	for (auto &a : o.attrs) {
		out += " " + a.name + "=\"";
		AppendXmlEscaped(a.value, true, out);
		out += "\"";
	}
	if (o.leaf) {
		out += ">";
		AppendXmlEscaped(o.text, false, out);
		out += "</" + o.ns + ":" + o.name + ">\n";
		return;
	}
	if (o.children.empty()) {
		out += "/>\n";
		return;
	}
	out += ">\n";
	for (auto &c : o.children) {
		XmlElement(c, depth + 1, out);
	}
	out.append(depth, '\t');
	out += "</" + o.ns + ":" + o.name + ">\n";
}

std::string XmlDocument(const Out &root) {
	std::string out = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
	XmlElement(root, 0, out);
	return out;
}

void Indent(size_t depth, std::string &out) {
	out.push_back('\n');
	out.append(depth * 2, ' ');
}

void JsonValueOf(const Out &o, size_t depth, std::string &out);

//! The members of an element's JSON object: children (repeated ones and
//! ARRAY ones as arrays, FLATTEN ones spliced in), then attributes.
void JsonObject(const Out &o, size_t depth, std::string &out) {
	std::vector<std::pair<std::string, std::vector<const Out *>>> members;
	std::vector<bool> arrays;
	std::vector<const Out *> children;
	for (auto &c : o.children) {
		if (c.flags & FLATTEN) {
			for (auto &g : c.children) {
				children.push_back(&g);
			}
		} else {
			children.push_back(&c);
		}
	}
	for (const Out *c : children) {
		const bool array = c->repeated || (c->flags & ARRAY);
		bool found = false;
		for (size_t m = 0; m < members.size(); m++) {
			if (members[m].first == c->name && arrays[m] && array) {
				members[m].second.push_back(c);
				found = true;
				break;
			}
		}
		if (!found) {
			members.emplace_back(c->name, std::vector<const Out *> {c});
			arrays.push_back(array);
		}
	}
	if (members.empty() && o.attrs.empty()) {
		out += "{}";
		return;
	}
	out += "{";
	bool first = true;
	for (size_t m = 0; m < members.size(); m++) {
		out += first ? "" : ",";
		first = false;
		Indent(depth + 1, out);
		AppendJsonString(members[m].first, out);
		out += ": ";
		if (!arrays[m]) {
			JsonValueOf(*members[m].second[0], depth + 1, out);
			continue;
		}
		out += "[";
		for (size_t i = 0; i < members[m].second.size(); i++) {
			out += i ? "," : "";
			Indent(depth + 2, out);
			JsonValueOf(*members[m].second[i], depth + 2, out);
		}
		Indent(depth + 1, out);
		out += "]";
	}
	for (auto &a : o.attrs) {
		out += first ? "" : ",";
		first = false;
		Indent(depth + 1, out);
		AppendJsonString(a.name, out);
		out += ": ";
		if (a.integer) {
			out += a.value;
		} else {
			AppendJsonString(a.value, out);
		}
	}
	Indent(depth, out);
	out += "}";
}

void JsonValueOf(const Out &o, size_t depth, std::string &out) {
	if (!o.leaf) {
		JsonObject(o, depth, out);
		return;
	}
	if (!(o.flags & VALUE)) {
		AppendJsonString(o.text, out);
		return;
	}
	out += "{";
	Indent(depth + 1, out);
	out += "\"value\": ";
	AppendJsonString(o.text, out);
	for (auto &a : o.attrs) {
		out += ",";
		Indent(depth + 1, out);
		AppendJsonString(a.name, out);
		out += ": ";
		AppendJsonString(a.value, out);
	}
	Indent(depth, out);
	out += "}";
}

std::string JsonDocument(const Out &root) {
	std::string out;
	JsonObject(root, 0, out);
	out += "\n";
	return out;
}

// ---------------------------------------------------------------------------
// Writers
// ---------------------------------------------------------------------------

//! The text of `field` in the first host of `hosts` that has one ("" if none).
std::string FirstHostValue(const std::vector<WriteRecord> &hosts, const std::string &field) {
	for (auto &h : hosts) {
		const std::string *v = h.Field(field);
		if (v) {
			return Trimmed(*v);
		}
	}
	return std::string();
}

//! A proposal's or resource item's hosts: with a `hosts` list, at most
//! MAX_RESEARCH_RESOURCE_HOSTS items and flat host_* fields that agree with it
//! (HostsConflict); the flat fields are then dropped, so the list alone is written.
void PrepareHosts(WriteRecord &r, const std::string &where) {
	const std::vector<WriteRecord> *hosts = r.List("hosts");
	if (!hosts) {
		return;
	}
	if (hosts->size() > MAX_RESEARCH_RESOURCE_HOSTS) {
		throw WriteError("research_resource: '" + where + "hosts' has " + std::to_string(hosts->size()) +
		                 " items, but research-resource-3.0.xsd allows at most " +
		                 std::to_string(MAX_RESEARCH_RESOURCE_HOSTS) + " hosts");
	}
	const std::string conflict = HostsConflict(r);
	if (!conflict.empty()) {
		const std::string flat = Trimmed(*r.Field(conflict));
		const std::string listed = FirstHostValue(*hosts, conflict);
		throw WriteError("research_resource: '" + where + conflict + "' is '" + flat + "', but the first '" + where +
		                 "hosts[]." + conflict + "' is " + (listed.empty() ? std::string("absent") : "'" + listed + "'") +
		                 ": give the 'hosts' list alone, or flat host_* fields that match it");
	}
	for (const char *field : kHostFields) {
		r.fields.erase(field);
	}
}

//! Fields a writer derives before rendering: a work's DOI when no external
//! ids were given; a peer review's subject identifier from the external_ids
//! item of container 'peer-review' (what orcid_external_ids yields for it); the
//! hosts of a research resource and of its items from their `hosts` lists.
WriteRecord Prepare(const std::string &writer, const WriteRecord &record) {
	WriteRecord r = record;
	if (writer == "research_resource") {
		PrepareHosts(r, std::string());
		auto items = r.lists.find("resource_items");
		if (items != r.lists.end()) {
			for (size_t i = 0; i < items->second.size(); i++) {
				PrepareHosts(items->second[i], "resource_items[" + std::to_string(i + 1) + "].");
			}
		}
	}
	if (writer == "funding" && r.List("contributors")) {
		// funding-3.0.xsd's contributor-attributes have no contributor-sequence:
		// refuse the value rather than drop it (yorc-rs: Error::Unwritable)
		const std::vector<WriteRecord> &contributors = *r.List("contributors");
		for (size_t i = 0; i < contributors.size(); i++) {
			if (contributors[i].Field("sequence")) {
				throw WriteError("funding: cannot write 'contributors[" + std::to_string(i + 1) +
				                 "].sequence' (funding/contributors/contributor[" + std::to_string(i + 1) +
				                 "]/contributor-attributes/contributor-sequence): a funding contributor has no "
				                 "contributor-sequence (funding-3.0.xsd)");
			}
		}
	}
	if (writer == "work" && !r.List("external_ids") && r.Field("doi")) {
		WriteRecord id;
		id.fields["type"] = "doi";
		id.fields["value"] = *r.Field("doi");
		id.fields["relationship"] = "self";
		r.lists["external_ids"] = {id};
	}
	if (writer == "peer_review" && r.List("external_ids")) {
		std::vector<WriteRecord> review;
		bool subject_seen = false;
		for (auto &id : *record.List("external_ids")) {
			const std::string *container = id.Field("container");
			if (!container || *container != "peer-review") {
				review.push_back(id);
				continue;
			}
			if (subject_seen) {
				throw WriteError("peer_review: more than one external_ids item has container 'peer-review' "
				                 "(the subject identifier)");
			}
			subject_seen = true;
			const char *const map[][2] = {{"type", "subject_id_type"},
			                              {"value", "subject_id_value"},
			                              {"url", "subject_id_url"},
			                              {"relationship", "subject_id_relationship"}};
			for (auto &m : map) {
				r.fields.erase(m[1]);
				if (id.Field(m[0])) {
					r.fields[m[1]] = *id.Field(m[0]);
				}
			}
		}
		r.lists["external_ids"] = std::move(review);
	}
	return r;
}

Node TemplateFor(const std::string &writer, const WriteRecord &record) {
	if (writer == "work") {
		return WorkTemplate();
	}
	if (writer == "funding") {
		return FundingTemplate();
	}
	if (writer == "affiliation") {
		const std::string *type = record.Field("affiliation_type");
		if (!type) {
			throw WriteError("affiliation: 'affiliation_type' is required (the document element: employment, "
			                 "education, ...)");
		}
		for (const char *t : kAffiliationTypes) {
			if (*type == t) {
				return AffiliationTemplate(*type);
			}
		}
		throw WriteError("affiliation: 'affiliation_type' must be one of distinction, education, employment, "
		                 "invited-position, membership, qualification, service (got '" +
		                 *type + "')");
	}
	if (writer == "peer_review") {
		return PeerReviewTemplate();
	}
	if (writer == "research_resource") {
		return ResearchResourceTemplate();
	}
	if (writer == "person_item") {
		const std::string *kind = record.Field("kind");
		if (!kind) {
			throw WriteError("person_item: 'kind' is required (address, keyword, other-name, researcher-url or "
			                 "external-identifier)");
		}
		return PersonItemTemplate(*kind);
	}
	if (writer == "group_id") {
		return GroupIdTemplate();
	}
	throw WriteError("unknown writer '" + writer + "'");
}

Out RenderItem(const std::string &writer, const WriteRecord &record, const std::string &where) {
	const Node root = TemplateFor(writer, record);
	return Renderer(writer).RenderRoot(root, Prepare(writer, record), where);
}

std::string Serialize(const std::string &writer, const Out &root, WriteFormat format) {
	try {
		return format == WriteFormat::Xml ? XmlDocument(root) : JsonDocument(root);
	} catch (const XmlError &e) {
		throw WriteError(writer + ": " + e.what());
	} catch (const JsonError &e) {
		throw WriteError(writer + ": " + e.what());
	}
}

//! The crosswalk emitter on the same tree (crosswalk.cpp), with the same error texts.
std::string SerializeCrosswalk(const std::string &writer, const Out &root, WriteFormat format) {
	try {
		return format == WriteFormat::Xml ? CrosswalkXml(root) : CrosswalkJson(root);
	} catch (const XmlError &e) {
		throw WriteError(writer + ": " + e.what());
	} catch (const JsonError &e) {
		throw WriteError(writer + ": " + e.what());
	} catch (const WriteError &e) {
		throw WriteError(writer + ": " + e.what());
	}
}

const RecordShape kExternalIdShape {"external_ids", {}};
const RecordShape kContributorShape {"contributors", {}};
const RecordShape kHostShape {"research_resource_hosts", {}};
const RecordShape kResourceItemShape {"research_resource_items",
                                      {{"external_ids", &kExternalIdShape}, {"hosts", &kHostShape}}};

struct WriterEntry {
	const char *name;
	RecordShape shape;
};

const std::vector<WriterEntry> &Writers() {
	static const std::vector<WriterEntry> writers = {
	    {"work", {"works", {{"external_ids", &kExternalIdShape}, {"contributors", &kContributorShape}}}},
	    {"funding", {"fundings", {{"external_ids", &kExternalIdShape}, {"contributors", &kContributorShape}}}},
	    {"affiliation", {"affiliations", {{"external_ids", &kExternalIdShape}}}},
	    {"peer_review", {"peer_reviews", {{"external_ids", &kExternalIdShape}}}},
	    {"research_resource",
	     {"research_resources",
	      {{"external_ids", &kExternalIdShape}, {"resource_items", &kResourceItemShape}, {"hosts", &kHostShape}}}},
	    {"person_item", {"person_items", {}}},
	    {"group_id", {"group_ids", {}}},
	};
	return writers;
}

} // namespace

std::string HostsConflict(const WriteRecord &record) {
	const std::vector<WriteRecord> *hosts = record.List("hosts");
	if (!hosts) {
		return std::string();
	}
	for (const char *field : kHostFields) {
		const std::string *flat = record.Field(field);
		if (flat && Trimmed(*flat) != FirstHostValue(*hosts, field)) {
			return field;
		}
	}
	return std::string();
}

const std::vector<std::string> &WriterNames() {
	static const std::vector<std::string> names = [] {
		std::vector<std::string> out;
		for (auto &w : Writers()) {
			out.push_back(w.name);
		}
		return out;
	}();
	return names;
}

const RecordShape *WriterShape(const std::string &writer) {
	for (auto &w : Writers()) {
		if (writer == w.name) {
			return &w.shape;
		}
	}
	return nullptr;
}

namespace write_tree {

Written RenderDocument(const std::string &writer, const WriteRecord &record) {
	return RenderItem(writer, record, std::string());
}

Written RenderBulkWorks(const std::vector<WriteRecord> &works) {
	if (works.empty()) {
		throw WriteError("bulk_works: a bulk request needs at least one work");
	}
	if (works.size() > MAX_BULK_WORKS) {
		throw WriteError("bulk_works: " + std::to_string(works.size()) + " works, but ORCID accepts at most " +
		                 std::to_string(MAX_BULK_WORKS) + " per bulk request; split them into batches of " +
		                 std::to_string(MAX_BULK_WORKS));
	}
	Out bulk;
	bulk.ns = "bulk";
	bulk.name = "bulk";
	for (size_t i = 0; i < works.size(); i++) {
		bulk.children.push_back(RenderItem("work", works[i], "works[" + std::to_string(i + 1) + "]."));
	}
	return bulk;
}

} // namespace write_tree

std::string WriteDocument(const std::string &writer, const WriteRecord &record, WriteFormat format) {
	return Serialize(writer, RenderItem(writer, record, std::string()), format);
}

std::string WriteDocument(const std::string &writer, const WriteRecord &record, WriteFormat format,
                          WriteStyle style) {
	if (style == WriteStyle::Native) {
		return WriteDocument(writer, record, format);
	}
	return SerializeCrosswalk(writer, RenderItem(writer, record, std::string()), format);
}

std::string WriteBulkWorks(const std::vector<WriteRecord> &works, WriteFormat format) {
	const Out bulk = write_tree::RenderBulkWorks(works);
	if (format == WriteFormat::Xml) {
		return Serialize("bulk_works", bulk, format);
	}
	try {
		std::string out = "{\n  \"bulk\": [";
		for (size_t i = 0; i < bulk.children.size(); i++) {
			out += i ? "," : "";
			out += "\n    {\n      \"work\": ";
			JsonObject(bulk.children[i], 3, out);
			out += "\n    }";
		}
		out += "\n  ]\n}\n";
		return out;
	} catch (const JsonError &e) {
		throw WriteError(std::string("bulk_works: ") + e.what());
	}
}

std::string WriteBulkWorks(const std::vector<WriteRecord> &works, WriteFormat format, WriteStyle style) {
	if (style == WriteStyle::Native) {
		return WriteBulkWorks(works, format);
	}
	return SerializeCrosswalk("bulk_works", write_tree::RenderBulkWorks(works), format);
}

} // namespace orcid
