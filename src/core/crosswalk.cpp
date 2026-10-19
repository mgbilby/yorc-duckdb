// The crosswalk emitter: the writers' output tree (orcid/write_tree.hpp) laid
// out exactly as yorc-rs writes the same item document, so that
// yorc-duckdb, yorc-rs and yorc-zig can be compared byte for byte
// (docs/REFERENCE.md, "Crosswalk mode"; cross-implementation review #5).
//
// The contract is yorc-rs's (yorc-rs/src/xml/write.rs and
// src/json.rs, `to_string`, for documents without ORCID's read-only fields;
// yorc-zig matches it byte for byte):
//
// XML (write.rs, module docs and the `Xml` emitter)
// - `<?xml version="1.0" encoding="UTF-8"?>` and LF, then the document
//   element; LF line ends; two spaces per level; a final LF;
// - every namespace used, declared once on the document element, before its
//   attributes, in `xml::Ns` order. That order is alphabetical by prefix, which
//   is byte order, so a std::set of the prefixes gives it;
// - elements in XSD sequence order and attributes `put-code`, then
//   `visibility` (the template order of writer.cpp, the same as yorc-rs's
//   for payloads, which carry no `path` or `display-index`);
// - an element with no children and no text as `<x/>`; text inline;
// - escaping: `&` `<` `>` and CR (`&#13;`) in text, also `"` TAB LF (`&quot;`
//   `&#9;` `&#10;`) in attributes, double-quoted: AppendXmlEscaped, which is
//   the same rule set (write.rs `escape`);
// - put-code in decimal as an i64 (`push_int`): no leading zeros; fuzzy-date
//   parts zero-padded (`{:04}` / `{:02}`), which the renderer already does for
//   the range it accepts.
//
// JSON (json.rs `to_string`: serde_json::to_string_pretty of a Value whose
// maps are BTreeMaps, then a final LF)
// - keys sorted by byte order; two-space indentation; `"key": value`; `{}`
//   and `[]` for empty containers; no nulls;
// - the same {"value": ...} wrappers and arrays as the default writer (both
//   follow ORCID's JSON); put-code a number;
// - containers yorc-rs always writes (`req_ext_ids`: a proposal's and a
//   resource item's `external-ids`, a peer review's `review-identifiers`)
//   hold `"external-id": []` when empty, where the default writer writes `{}`;
// - strings as serde_json escapes them: DEL and U+2028 / U+2029 raw.

#include "orcid/write_tree.hpp"

#include "orcid/json.hpp"
#include "orcid/xml.hpp"

#include <algorithm>
#include <set>

namespace orcid {
namespace write_tree {

namespace {

// ---------------------------------------------------------------------------
// XML
// ---------------------------------------------------------------------------

void CollectPrefixes(const Written &o, std::set<std::string> &out) {
	out.insert(o.ns);
	for (auto &c : o.children) {
		CollectPrefixes(c, out);
	}
}

//! An attribute value as yorc-rs prints it: integers through its i64.
std::string AttrValue(const WrittenAttr &a) {
	if (!a.integer) {
		return a.value;
	}
	std::string out;
	if (!CrosswalkInteger(a.value, out)) {
		throw WriteError("'" + a.name + "' is " + a.value + ", outside yorc-rs's put-code range (an i64)");
	}
	return out;
}

void XmlElement(const Written &o, size_t depth, std::string &out) {
	out.append(2 * depth, ' ');
	out += "<";
	out += o.ns;
	out += ":";
	out += o.name;
	if (depth == 0) {
		std::set<std::string> prefixes;
		CollectPrefixes(o, prefixes);
		for (auto &p : prefixes) {
			out += " xmlns:";
			out += p;
			out += "=\"http://www.orcid.org/ns/";
			out += p;
			out += "\"";
		}
	}
	for (auto &a : o.attrs) {
		out += " ";
		out += a.name;
		out += "=\"";
		AppendXmlEscaped(AttrValue(a), true, out);
		out += "\"";
	}
	if (o.leaf) {
		out += ">";
		AppendXmlEscaped(o.text, false, out);
		out += "</";
		out += o.ns;
		out += ":";
		out += o.name;
		out += ">\n";
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
	out.append(2 * depth, ' ');
	out += "</";
	out += o.ns;
	out += ":";
	out += o.name;
	out += ">\n";
}

// ---------------------------------------------------------------------------
// JSON: the tree as a JsonValue (orcid/json.hpp), keys then sorted
// ---------------------------------------------------------------------------

JsonValue String(const std::string &text) {
	JsonValue v;
	v.type = JsonValue::Type::String;
	v.text = text;
	return v;
}

JsonValue Number(const std::string &text) {
	JsonValue v;
	v.type = JsonValue::Type::Number;
	v.text = text;
	return v;
}

JsonValue Object() {
	JsonValue v;
	v.type = JsonValue::Type::Object;
	return v;
}

JsonValue Array() {
	JsonValue v;
	v.type = JsonValue::Type::Array;
	return v;
}

//! Keys in byte order, as a BTreeMap<String, Value> holds them (stable, so
//! the order of a repeated key, which no template produces, would be kept).
void SortKeys(JsonValue &object) {
	std::stable_sort(object.members.begin(), object.members.end(),
	                 [](const std::pair<std::string, JsonValue> &a, const std::pair<std::string, JsonValue> &b) {
		                 return a.first < b.first;
	                 });
}

JsonValue ValueOf(const Written &o);

//! An element's object: the members the default writer writes (children,
//! repeated and ARRAY ones as arrays, FLATTEN ones spliced in; attributes),
//! plus an empty array for each list that wrote nothing, then sorted.
JsonValue ObjectOf(const Written &o) {
	std::vector<const Written *> children;
	std::vector<const std::string *> empty;
	for (auto &c : o.children) {
		if (c.flags & FLATTEN) {
			for (auto &g : c.children) {
				children.push_back(&g);
			}
			for (auto &e : c.empty_lists) {
				empty.push_back(&e);
			}
		} else {
			children.push_back(&c);
		}
	}
	for (auto &e : o.empty_lists) {
		empty.push_back(&e);
	}
	JsonValue obj = Object();
	std::vector<bool> arrays;
	for (const Written *c : children) {
		const bool array = c->repeated || (c->flags & ARRAY);
		bool found = false;
		for (size_t m = 0; m < obj.members.size(); m++) {
			if (obj.members[m].first == c->name && arrays[m] && array) {
				obj.members[m].second.items.push_back(ValueOf(*c));
				found = true;
				break;
			}
		}
		if (found) {
			continue;
		}
		if (array) {
			JsonValue a = Array();
			a.items.push_back(ValueOf(*c));
			obj.members.emplace_back(c->name, std::move(a));
		} else {
			obj.members.emplace_back(c->name, ValueOf(*c));
		}
		arrays.push_back(array);
	}
	for (const std::string *name : empty) {
		bool present = false;
		for (auto &m : obj.members) {
			present = present || m.first == *name;
		}
		if (!present) {
			obj.members.emplace_back(*name, Array());
			arrays.push_back(true);
		}
	}
	for (auto &a : o.attrs) {
		obj.members.emplace_back(a.name, a.integer ? Number(AttrValue(a)) : String(a.value));
	}
	SortKeys(obj);
	return obj;
}

JsonValue ValueOf(const Written &o) {
	if (!o.leaf) {
		return ObjectOf(o);
	}
	if (!(o.flags & VALUE)) {
		return String(o.text);
	}
	JsonValue obj = Object();
	obj.members.emplace_back("value", String(o.text));
	for (auto &a : o.attrs) {
		obj.members.emplace_back(a.name, String(a.value));
	}
	SortKeys(obj);
	return obj;
}

//! serde_json's PrettyFormatter: "{" / "[", each entry on its own line
//! indented two spaces per level, "," before the line break, the closing
//! bracket on its own line; an empty object or array stays "{}" / "[]".
void Pretty(const JsonValue &v, size_t depth, std::string &out) {
	switch (v.type) {
	case JsonValue::Type::String:
		AppendSerdeJsonString(v.text, out);
		return;
	case JsonValue::Type::Number:
	case JsonValue::Type::Bool:
		out += v.text;
		return;
	case JsonValue::Type::Null:
		out += "null"; // never built: absent values are left out
		return;
	case JsonValue::Type::Array:
		if (v.items.empty()) {
			out += "[]";
			return;
		}
		out += "[";
		for (size_t i = 0; i < v.items.size(); i++) {
			out += i ? ",\n" : "\n";
			out.append(2 * (depth + 1), ' ');
			Pretty(v.items[i], depth + 1, out);
		}
		out += "\n";
		out.append(2 * depth, ' ');
		out += "]";
		return;
	case JsonValue::Type::Object:
		if (v.members.empty()) {
			out += "{}";
			return;
		}
		out += "{";
		for (size_t i = 0; i < v.members.size(); i++) {
			out += i ? ",\n" : "\n";
			out.append(2 * (depth + 1), ' ');
			AppendSerdeJsonString(v.members[i].first, out);
			out += ": ";
			Pretty(v.members[i].second, depth + 1, out);
		}
		out += "\n";
		out.append(2 * depth, ' ');
		out += "}";
		return;
	}
}

} // namespace

bool CrosswalkInteger(const std::string &text, std::string &out) {
	size_t i = 0;
	const bool negative = !text.empty() && text[0] == '-';
	if (negative) {
		i = 1;
	}
	if (i >= text.size()) {
		return false;
	}
	for (size_t k = i; k < text.size(); k++) {
		if (text[k] < '0' || text[k] > '9') {
			return false;
		}
	}
	while (i + 1 < text.size() && text[i] == '0') {
		i++;
	}
	std::string digits = text.substr(i);
	// i64: -9223372036854775808 ..= 9223372036854775807
	const std::string limit = negative ? "9223372036854775808" : "9223372036854775807";
	if (digits.size() > limit.size() || (digits.size() == limit.size() && digits > limit)) {
		return false;
	}
	out = (negative && digits != "0") ? "-" + digits : digits;
	return true;
}

void AppendSerdeJsonString(const std::string &text, std::string &out) {
	static const char HEX[] = "0123456789abcdef";
	out.push_back('"');
	const char *p = text.data();
	const char *end = p + text.size();
	while (p < end) {
		const unsigned char c = static_cast<unsigned char>(*p);
		if (c >= 0x80) {
			const char *start = p;
			uint32_t cp;
			if (!DecodeUtf8(p, end, cp)) {
				throw JsonError("invalid UTF-8 at byte " + std::to_string(start - text.data()) + " of a string value");
			}
			out.append(start, static_cast<size_t>(p - start));
			continue;
		}
		p++;
		switch (c) {
		case '"':
			out += "\\\"";
			break;
		case '\\':
			out += "\\\\";
			break;
		case '\b':
			out += "\\b";
			break;
		case '\f':
			out += "\\f";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\t':
			out += "\\t";
			break;
		default:
			if (c < 0x20) {
				out += "\\u00";
				out.push_back(HEX[c >> 4]);
				out.push_back(HEX[c & 0xF]);
			} else {
				out.push_back(static_cast<char>(c)); // DEL (0x7F) included, as serde_json writes it
			}
			break;
		}
	}
	out.push_back('"');
}

std::string CrosswalkXml(const Written &root) {
	std::string out = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
	XmlElement(root, 0, out);
	return out;
}

std::string CrosswalkJson(const Written &root) {
	JsonValue doc;
	if (root.ns == "bulk" && root.name == "bulk") {
		// json.rs w_bulk: {"bulk": [{"work": {...}}, ...]}
		JsonValue items = Array();
		for (auto &work : root.children) {
			JsonValue entry = Object();
			entry.members.emplace_back("work", ObjectOf(work));
			items.items.push_back(std::move(entry));
		}
		doc = Object();
		doc.members.emplace_back("bulk", std::move(items));
	} else {
		doc = ObjectOf(root);
	}
	std::string out;
	Pretty(doc, 0, out);
	out += "\n";
	return out;
}

} // namespace write_tree
} // namespace orcid
