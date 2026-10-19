#include "orcid/json_map.hpp"

#include "orcid/id.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace orcid {

// ---------------------------------------------------------------------------
// JSON -> element tree. The rules are listed in json_map.hpp; they were
// derived by pairing the JSON and XML samples of reference/orcid-model
// (record_3.0/samples/write_samples and read_samples/record-full-3.0.json)
// and are documented in docs/REFERENCE.md ("JSON documents").
// ---------------------------------------------------------------------------

namespace {

//! Scalar members that are XML attributes of the element they belong to.
bool IsAttributeKey(const std::string &key) {
	return key == "put-code" || key == "path" || key == "visibility" || key == "display-index" ||
	       key == "verified" || key == "primary" || key == "num-found" || key == "transient" || key == "lang" ||
	       key == "xml:lang";
}

//! Members that may sit next to "value" in a value wrapper (simple-content
//! elements with attributes: translated-title, amount, external-id-normalized,
//! country).
bool IsWrapperAttribute(const std::string &key) {
	return key == "language-code" || key == "currency-code" || key == "transient" || key == "visibility";
}

std::string AttributeName(const std::string &key) {
	return key == "lang" ? std::string("xml:lang") : key;
}

bool IsValueWrapper(const JsonValue &v) {
	if (v.type != JsonValue::Type::Object) {
		return false;
	}
	const JsonValue *value = v.Find("value");
	if (!value || !(value->IsScalar() || value->type == JsonValue::Type::Null)) {
		return false;
	}
	for (auto &m : v.members) {
		if (m.first == "value") {
			continue;
		}
		if (!IsWrapperAttribute(m.first) || !(m.second.IsScalar() || m.second.type == JsonValue::Type::Null)) {
			return false;
		}
	}
	return true;
}

bool EndsWith(const std::string &s, const char *suffix) {
	const size_t n = std::strlen(suffix);
	return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

//! A JSON integer literal that fits int64 comfortably (epoch milliseconds).
bool AsInteger(const JsonValue &v, int64_t &out) {
	if (v.type != JsonValue::Type::Number || v.text.empty() || v.text.size() > 18) {
		return false;
	}
	size_t i = v.text[0] == '-' ? 1 : 0;
	if (i == v.text.size()) {
		return false;
	}
	int64_t n = 0;
	for (; i < v.text.size(); i++) {
		if (v.text[i] < '0' || v.text[i] > '9') {
			return false;
		}
		n = n * 10 + (v.text[i] - '0');
	}
	out = v.text[0] == '-' ? -n : n;
	return true;
}

void AddMember(Element &parent, const std::string &key, const JsonValue &v);

// XSD sequence order of the children of the containers whose children of
// different names feed one row table (or the timeline) in document order.
// JSON objects carry no order that can be relied on: yorc-rs and yorc-zig
// write their keys sorted, so `person` would otherwise give person_items in
// key order (addresses, emails, external-identifiers, keywords, other-names,
// researcher-urls) where the XML gives the XSD's (CROSSCHECK #15). Only the
// children named here move, and only among the positions they hold; children
// of the same name keep their document order. Containers whose XSD order is
// already alphabetical are left out: activities-summary (distinctions ...
// works), the activity groups (external-ids before the items), peer-review
// (review-identifiers before subject-external-identifier).
struct ChildOrder {
	const char *parent;
	const char *children[16]; //!< nullptr-terminated
};

const ChildOrder kChildOrders[] = {
    // record-3.0.xsd, record:record
    {"record", {"orcid-identifier", "preferences", "history", "person", "activities-summary"}},
    // person-3.0.xsd, person:person
    {"person",
     {"last-modified-date", "name", "other-names", "biography", "researcher-urls", "emails", "addresses", "keywords",
      "external-identifiers"}},
    // personal-details-3.0.xsd, personal-details:personal-details
    {"personal-details", {"last-modified-date", "name", "other-names", "biography"}},
    // summary-3.0.xsd, summary:record-summary
    {"record-summary",
     {"created-date", "last-modified-date", "credit-name", "orcid-identifier", "external-identifiers", "employments",
      "professional-activities", "fundings", "works", "peer-reviews", "education-qualifications", "research-resources",
      "email-domains"}},
    // research-resource-3.0.xsd: the proposal before the items (resource-items
    // is made at the first "resource-item", wherever that key is)
    {"research-resource", {"proposal", "resource-items"}},
};

int ChildRank(const ChildOrder &order, const std::string &name) {
	for (int i = 0; i < 16 && order.children[i]; i++) {
		if (name == order.children[i]) {
			return i;
		}
	}
	return -1;
}

//! Puts the children of `e` that kChildOrders lists for it in XSD order.
void OrderChildren(Element &e) {
	for (auto &order : kChildOrders) {
		if (e.name != order.parent) {
			continue;
		}
		std::vector<size_t> positions;
		std::vector<Element> known;
		for (size_t i = 0; i < e.children.size(); i++) {
			if (ChildRank(order, e.children[i].name) >= 0) {
				positions.push_back(i);
				known.push_back(std::move(e.children[i]));
			}
		}
		std::stable_sort(known.begin(), known.end(), [&](const Element &a, const Element &b) {
			return ChildRank(order, a.name) < ChildRank(order, b.name);
		});
		for (size_t k = 0; k < positions.size(); k++) {
			e.children[positions[k]] = std::move(known[k]);
		}
		return;
	}
}

//! history/creation-method: ORCID's JSON has the Java enum names, the XML
//! (and the rows) the XSD values. As yorc-rs's creation_method_from_json: the
//! four names map, anything else (an XSD value included) is kept as it is.
std::string CreationMethodFromJson(const std::string &text) {
	const size_t b = text.find_first_not_of(" \t\r\n");
	const size_t e = text.find_last_not_of(" \t\r\n");
	const std::string t = b == std::string::npos ? std::string() : text.substr(b, e - b + 1);
	if (t == "API") {
		return "API";
	}
	if (t == "DIRECT") {
		return "Direct";
	}
	if (t == "MEMBER_REFERRED") {
		return "Member-referred";
	}
	if (t == "WEBSITE") {
		return "website";
	}
	return text;
}

//! Fills `e` (already named `key`) from an object or a value wrapper.
void FillElement(Element &e, const std::string &key, const JsonValue &obj) {
	if (IsValueWrapper(obj)) {
		for (auto &m : obj.members) {
			if (m.second.type == JsonValue::Type::Null) {
				continue;
			}
			if (m.first != "value") {
				e.attrs.emplace_back(m.first, m.second.text);
				continue;
			}
			int64_t ms;
			if (EndsWith(key, "-date") && AsInteger(m.second, ms)) {
				e.text = EpochMillisToIso(ms);
			} else {
				e.text = m.second.text;
			}
		}
		return;
	}
	// Identifier objects (orcid-id, client-id: uri, path, host) carry path as an element.
	const bool identifier = obj.Has("uri") || obj.Has("host");
	for (auto &m : obj.members) {
		const JsonValue &v = m.second;
		if (v.type == JsonValue::Type::Null) {
			continue;
		}
		if ((m.first == "summaries" || m.first == "bulk") && v.type == JsonValue::Type::Array) {
			for (auto &wrapper : v.items) {
				for (auto &inner : wrapper.members) {
					AddMember(e, inner.first, inner.second);
				}
			}
			continue;
		}
		if (v.IsScalar() && IsAttributeKey(m.first) && !(m.first == "path" && identifier)) {
			e.attrs.emplace_back(AttributeName(m.first), v.text);
			if (m.first != "put-code") {
				continue;
			}
		}
		AddMember(e, m.first, v);
	}
	OrderChildren(e);
}

//! Appends the element(s) member `key` maps to.
void AddMember(Element &parent, const std::string &key, const JsonValue &v) {
	switch (v.type) {
	case JsonValue::Type::Null:
		return;
	case JsonValue::Type::Array:
		for (auto &item : v.items) {
			AddMember(parent, key, item);
		}
		return;
	default:
		break;
	}
	if (key == "resource-item" && parent.name != "resource-items") {
		Element *wrapper = nullptr;
		for (auto &c : parent.children) {
			if (c.name == "resource-items") {
				wrapper = &c;
			}
		}
		if (!wrapper) {
			parent.children.emplace_back();
			parent.children.back().name = "resource-items";
			wrapper = &parent.children.back();
		}
		AddMember(*wrapper, key, v);
		return;
	}
	parent.children.emplace_back();
	Element &e = parent.children.back();
	e.name = key;
	int64_t ms;
	if (v.type == JsonValue::Type::Object) {
		FillElement(e, key, v);
	} else if (EndsWith(key, "-date") && AsInteger(v, ms)) {
		// a bare epoch-millisecond timestamp ("created-date": 1540896558250),
		// converted as the {"value": ...} form is (yorc-rs's R::time takes both)
		e.text = EpochMillisToIso(ms);
	} else {
		e.text = v.text;
	}
	if (key == "creation-method" && parent.name == "history") {
		e.text = CreationMethodFromJson(e.text);
	}
}

// ---------------------------------------------------------------------------
// Document element inference
// ---------------------------------------------------------------------------

struct PathKind {
	const char *segment;    //!< path segment after the iD
	const char *collection; //!< document element without a put-code after it
	const char *item;       //!< with one
};

const PathKind kPathKinds[] = {
    {"activities", "activities-summary", "activities-summary"},
    {"person", "person", "person"},
    {"personal-details", "personal-details", "personal-details"},
    {"biography", "biography", "biography"},
    {"history", "history", "history"},
    {"preferences", "preferences", "preferences"},
    {"summary", "record-summary", "record-summary"},
    {"email", "emails", "email"},
    {"address", "addresses", "address"},
    {"other-names", "other-names", "other-name"},
    {"researcher-urls", "researcher-urls", "researcher-url"},
    {"keywords", "keywords", "keyword"},
    {"external-identifiers", "external-identifiers", "external-identifier"},
    {"works", "works", "bulk"},
    {"work", "work", "work"},
    {"fundings", "fundings", "funding"},
    {"funding", "funding", "funding"},
    {"peer-reviews", "peer-reviews", "peer-review"},
    {"peer-review", "peer-review", "peer-review"},
    {"research-resources", "research-resources", "research-resource"},
    {"research-resource", "research-resource", "research-resource"},
    {"distinctions", "distinctions", "distinction"},
    {"distinction", "distinction", "distinction"},
    {"educations", "educations", "education"},
    {"education", "education", "education"},
    {"employments", "employments", "employment"},
    {"employment", "employment", "employment"},
    {"invited-positions", "invited-positions", "invited-position"},
    {"invited-position", "invited-position", "invited-position"},
    {"memberships", "memberships", "membership"},
    {"membership", "membership", "membership"},
    {"qualifications", "qualifications", "qualification"},
    {"qualification", "qualification", "qualification"},
    {"services", "services", "service"},
    {"service", "service", "service"},
};

//! "/0000-0002-7319-2192/other-names/4923" -> other-name; "/<iD>" -> record.
std::string RootFromPath(const std::string &path) {
	std::vector<std::string> segs;
	size_t start = 0;
	while (start <= path.size()) {
		size_t slash = path.find('/', start);
		size_t end = slash == std::string::npos ? path.size() : slash;
		if (end > start) {
			segs.push_back(path.substr(start, end - start));
		}
		if (slash == std::string::npos) {
			break;
		}
		start = slash + 1;
	}
	size_t i = 0;
	if (!segs.empty() && segs[0].size() == 19 && IsValidId(segs[0])) {
		i = 1;
	}
	if (i == segs.size()) {
		return i == 1 ? std::string("record") : std::string();
	}
	const bool item = segs.size() > i + 1;
	for (auto &k : kPathKinds) {
		if (segs[i] == k.segment) {
			return item ? k.item : k.collection;
		}
	}
	return std::string();
}

std::string Plural(const std::string &affiliation) {
	return affiliation + "s"; // distinction, education, employment, invited-position, membership, qualification, service
}

//! The collection a "group" / "affiliation-group" array belongs to, from the
//! item elements of its first group.
std::string RootFromGroups(const JsonValue &object) {
	const JsonValue *groups = object.Find("group");
	if (groups && groups->type == JsonValue::Type::Array) {
		for (auto &g : groups->items) {
			if (g.Has("work-summary")) {
				return "works";
			}
			if (g.Has("funding-summary")) {
				return "fundings";
			}
			if (g.Has("peer-review-group")) {
				return "peer-reviews";
			}
			if (g.Has("research-resource-summary")) {
				return "research-resources";
			}
		}
	}
	const JsonValue *affiliation_groups = object.Find("affiliation-group");
	if (affiliation_groups && affiliation_groups->type == JsonValue::Type::Array) {
		for (auto &g : affiliation_groups->items) {
			const JsonValue *summaries = g.Find("summaries");
			if (!summaries || summaries->type != JsonValue::Type::Array) {
				continue;
			}
			for (auto &w : summaries->items) {
				for (auto &m : w.members) {
					if (EndsWith(m.first, "-summary")) {
						return Plural(m.first.substr(0, m.first.size() - std::strlen("-summary")));
					}
				}
			}
		}
	}
	return std::string();
}

bool IsArrayMember(const JsonValue &object, const char *key) {
	const JsonValue *v = object.Find(key);
	return v && v->type == JsonValue::Type::Array;
}

//! Members every element-summary item may carry besides its own (read side).
bool IsItemMetadata(const std::string &key) {
	return key == "created-date" || key == "last-modified-date" || key == "source" || key == "visibility" ||
	       key == "path" || key == "put-code" || key == "display-index";
}

//! True when every member of `o` is item metadata or one of `keys`, so that a
//! shape named by a common word ("url", "country", "name") is only taken for
//! an ORCID item when nothing else is there.
bool OnlyKeys(const JsonValue &o, std::initializer_list<const char *> keys) {
	for (auto &m : o.members) {
		if (IsItemMetadata(m.first)) {
			continue;
		}
		bool known = false;
		for (const char *k : keys) {
			known = known || m.first == k;
		}
		if (!known) {
			return false;
		}
	}
	return true;
}

} // namespace

std::string InferJsonRoot(const JsonValue &o) {
	if (o.type != JsonValue::Type::Object) {
		return std::string();
	}
	auto has = [&](const char *key) { return o.Has(key); };
	if (has("bulk")) {
		return "bulk";
	}
	const JsonValue *path = o.Find("path");
	if (path && path->type == JsonValue::Type::String && !has("uri") && !has("host")) {
		std::string root = RootFromPath(path->text);
		if (!root.empty()) {
			return root;
		}
	}
	// Documents named by members only ORCID uses.
	if (has("orcid-identifier") &&
	    (has("activities-summary") || has("person") || has("history") || has("preferences"))) {
		return "record";
	}
	if (has("orcid-identifier") && (has("employments") || has("professional-activities") ||
	                                 has("education-qualifications") || has("email-domains") ||
	                                 has("external-identifiers") || has("works") || has("fundings") ||
	                                 has("peer-reviews") || has("research-resources"))) {
		return "record-summary";
	}
	if (has("distinctions") || has("educations") || has("invited-positions") || has("memberships") ||
	    has("qualifications") || has("services")) {
		return "activities-summary";
	}
	if (has("response-code") || has("developer-message")) {
		return "error";
	}
	if (has("notification-type")) {
		return "notification";
	}
	if (IsArrayMember(o, "notification")) {
		return "notifications";
	}
	if (has("group-id-record")) {
		return "group-id-records";
	}
	if (has("group-id")) {
		return "group-id-record";
	}
	if (has("expanded-result")) {
		return "expanded-search";
	}
	if (has("num-found") || (has("result") && OnlyKeys(o, {"result", "num-found"}))) {
		return "search";
	}
	if (has("reviewer-role") || has("review-group-id") || has("review-identifiers")) {
		return "peer-review";
	}
	if (has("proposal") || has("resource-item") || has("resource-items")) {
		return "research-resource";
	}
	// a funding's organization is required, and only a funding has both an organization and a title or type
	if (has("amount") || has("organization-defined-type") || (has("organization") && (has("title") || has("type")))) {
		return "funding";
	}
	// a work's title and type are required (ORCID writes both, null or not)
	if (has("title") && (has("type") || has("journal-title") || has("citation") || has("publication-date"))) {
		return "work";
	}
	if ((has("department-name") || has("role-title") || has("organization")) &&
	    OnlyKeys(o, {"department-name", "role-title", "start-date", "end-date", "organization", "url", "external-ids"})) {
		throw JsonRootError("the JSON document is an affiliation, and ORCID's JSON does not say which type: "
		                    "pass json_root := 'employment' (or 'education', 'qualification', "
		                    "'invited-position', 'distinction', 'membership', 'service')");
	}
	if (has("creation-method") || has("verified-primary-email") || has("submission-date")) {
		return "history";
	}
	const std::initializer_list<const char *> person = {"name",   "other-names", "biography", "researcher-urls",
	                                                    "emails", "addresses",   "keywords",  "external-identifiers"};
	if ((has("researcher-urls") || has("emails") || has("addresses") || has("keywords") ||
	     has("external-identifiers")) &&
	    OnlyKeys(o, person)) {
		return "person";
	}
	if ((has("name") || has("other-names")) && OnlyKeys(o, {"name", "other-names", "biography"})) {
		return "personal-details";
	}
	std::string collection = RootFromGroups(o);
	if (!collection.empty()) {
		return collection;
	}
	for (const char *item : {"other-name", "researcher-url", "email", "address", "keyword", "external-identifier"}) {
		if (IsArrayMember(o, item) && OnlyKeys(o, {item})) {
			return std::string(item) == "address" ? std::string("addresses") : std::string(item) + "s";
		}
	}
	const JsonValue *email = o.Find("email");
	if (email && email->type == JsonValue::Type::String && OnlyKeys(o, {"email", "verified", "primary"})) {
		return "email";
	}
	if (has("url") && OnlyKeys(o, {"url-name", "url"})) {
		return "researcher-url";
	}
	if ((has("external-id-type") || has("external-id-value")) &&
	    OnlyKeys(o, {"external-id-type", "external-id-value", "external-id-normalized", "external-id-normalized-error",
	                 "external-id-url", "external-id-relationship"})) {
		return "external-identifier";
	}
	if (has("country") && OnlyKeys(o, {"country"})) {
		return "address";
	}
	if (has("content") && OnlyKeys(o, {"content"})) {
		throw JsonRootError("the JSON document is an other name or a keyword, and ORCID's JSON does not say "
		                    "which: pass json_root := 'other-name' or 'keyword'");
	}
	if (has("locale") && OnlyKeys(o, {"locale"})) {
		return "preferences";
	}
	return std::string();
}

void JsonToElements(const JsonValue &doc, const std::string &root, std::vector<Element> &out) {
	auto one = [&](const JsonValue &object) {
		Element e;
		e.name = root.empty() ? InferJsonRoot(object) : root;
		FillElement(e, e.name, object);
		out.push_back(std::move(e));
	};
	if (doc.type == JsonValue::Type::Object) {
		one(doc);
	} else if (doc.type == JsonValue::Type::Array) {
		for (auto &item : doc.items) {
			if (item.type == JsonValue::Type::Object) {
				one(item);
			}
		}
	}
}

std::string EpochMillisToIso(int64_t ms) {
	auto floor_div = [](int64_t a, int64_t b) { return a / b - ((a % b != 0) && ((a < 0) != (b < 0))); };
	const int64_t secs = floor_div(ms, 1000);
	const int64_t millis = ms - secs * 1000;
	int64_t days = floor_div(secs, 86400);
	const int64_t sod = secs - days * 86400;
	// civil_from_days (H. Hinnant, "chrono-Compatible Low-Level Date Algorithms")
	days += 719468;
	const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
	const int64_t doe = days - era * 146097;
	const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	int64_t year = yoe + era * 400;
	const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	const int64_t mp = (5 * doy + 2) / 153;
	const int64_t day = doy - (153 * mp + 2) / 5 + 1;
	const int64_t month = mp < 10 ? mp + 3 : mp - 9;
	year += month <= 2;
	char buf[96]; // 83 bytes is the widest int64 output; sized for -Wformat-truncation
	std::snprintf(buf, sizeof(buf), "%04lld-%02lld-%02lldT%02lld:%02lld:%02lld.%03lldZ", static_cast<long long>(year),
	              static_cast<long long>(month), static_cast<long long>(day), static_cast<long long>(sod / 3600),
	              static_cast<long long>(sod / 60 % 60), static_cast<long long>(sod % 60),
	              static_cast<long long>(millis));
	return buf;
}

// ---------------------------------------------------------------------------
// ElementReader
// ---------------------------------------------------------------------------

XmlEvent ElementReader::Next() {
	if (!started_) {
		started_ = true;
		stack_.push_back({root_, 0, false});
		current_ = root_;
		return XmlEvent::StartElement;
	}
	while (!stack_.empty()) {
		Position &top = stack_.back();
		if (!top.text_done) {
			top.text_done = true;
			if (!top.element->text.empty()) {
				current_ = top.element;
				return XmlEvent::Text;
			}
		}
		if (top.next_child < top.element->children.size()) {
			const Element *child = &top.element->children[top.next_child++];
			stack_.push_back({child, 0, false});
			current_ = child;
			return XmlEvent::StartElement;
		}
		current_ = top.element;
		stack_.pop_back();
		return XmlEvent::EndElement;
	}
	current_ = nullptr;
	return XmlEvent::End;
}

bool ElementReader::Attribute(const char *qname, std::string &out) const {
	if (!current_) {
		return false;
	}
	for (auto &kv : current_->attrs) {
		if (kv.first == qname) {
			out = kv.second;
			return true;
		}
	}
	return false;
}

} // namespace orcid
