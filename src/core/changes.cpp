#include "orcid/changes.hpp"

#include "orcid/sections.hpp"

#include <initializer_list>
#include <map>
#include <unordered_map>

namespace orcid {

namespace {

//! Columns that never count as a change.
bool Ignored(const std::string &column) {
	return column == "source_hash" || column == "item";
}

//! Column giving the item kind, for sections that mix kinds.
int KindColumn(const SectionSpec &spec) {
	for (size_t c = 0; c < spec.columns.size(); c++) {
		auto &n = spec.columns[c].name;
		if (n == "affiliation_type" || n == "kind") {
			return static_cast<int>(c);
		}
	}
	return -1;
}

int Column(const SectionSpec &spec, const char *name) {
	for (size_t c = 0; c < spec.columns.size(); c++) {
		if (spec.columns[c].name == name) {
			return static_cast<int>(c);
		}
	}
	return -1;
}

std::optional<std::string> Label(const SectionSpec &spec, const Row &row) {
	for (const char *name : {"title", "role_title", "value", "subject_name", "org_name", "convening_org_name",
	                         "given_names", "creation_method"}) {
		int c = Column(spec, name);
		if (c >= 0 && row[c]) {
			return row[c];
		}
	}
	return std::nullopt;
}

struct Keyed {
	std::string key;
	std::string kind;
	std::optional<std::string> put_code;
	const Row *row;
};

//! Keys rows by kind + put-code; a repeated key gets an occurrence suffix so
//! duplicates pair up in order instead of colliding.
std::vector<Keyed> KeyRows(const SectionSpec &spec, const std::vector<Row> &rows) {
	int kind_col = KindColumn(spec);
	int put_col = Column(spec, "put_code");
	std::unordered_map<std::string, int> seen;
	std::vector<Keyed> out;
	for (auto &row : rows) {
		Keyed k;
		k.kind = kind_col >= 0 && row[kind_col] ? *row[kind_col] : std::string();
		if (put_col >= 0) {
			k.put_code = row[put_col];
		}
		std::string base = k.kind + "\x1f" + (k.put_code ? *k.put_code : std::string("\x1e"));
		int n = seen[base]++;
		k.key = base + "\x1f" + std::to_string(n);
		k.row = &row;
		out.push_back(std::move(k));
	}
	return out;
}

} // namespace

const std::vector<std::string> &DiffSections() {
	static const std::vector<std::string> sections = {"person",       "person_items",       "works",
	                                                  "fundings",     "affiliations",       "peer_reviews",
	                                                  "research_resources", "history"};
	return sections;
}

void DiffDocuments(const char *old_xml, size_t old_len, const char *new_xml, size_t new_len,
                   std::vector<ChangeRow> &out) {
	DiffDocuments(old_xml, old_len, DocFormat::Xml, new_xml, new_len, DocFormat::Xml, std::string(), out);
}

void DiffDocuments(const char *old_doc, size_t old_len, DocFormat old_format, const char *new_doc, size_t new_len,
                   DocFormat new_format, const std::string &json_root, std::vector<ChangeRow> &out) {
	for (auto &name : DiffSections()) {
		const SectionSpec *spec = FindSection(name);
		if (!spec) {
			continue;
		}
		std::vector<Row> old_rows, new_rows;
		ExtractDocument(*spec, old_doc, old_len, old_format, json_root, old_rows);
		ExtractDocument(*spec, new_doc, new_len, new_format, json_root, new_rows);
		auto old_keyed = KeyRows(*spec, old_rows);
		auto new_keyed = KeyRows(*spec, new_rows);
		std::map<std::string, const Keyed *> old_by_key;
		for (auto &k : old_keyed) {
			old_by_key[k.key] = &k;
		}
		const int orcid_col = Column(*spec, "orcid");
		auto orcid_of = [&](const Row &row) {
			return orcid_col >= 0 && row[orcid_col] ? *row[orcid_col] : std::string();
		};
		auto base = [&](const Keyed &k, const Row &row, const char *change) {
			ChangeRow c;
			c.orcid = orcid_of(row);
			c.section = name;
			c.kind = k.kind;
			c.put_code = k.put_code;
			c.change = change;
			c.label = Label(*spec, row);
			return c;
		};

		std::map<std::string, bool> matched;
		for (auto &k : new_keyed) {
			auto it = old_by_key.find(k.key);
			if (it == old_by_key.end()) {
				out.push_back(base(k, *k.row, "added"));
				continue;
			}
			matched[k.key] = true;
			const Row &o = *it->second->row;
			const Row &n = *k.row;
			for (size_t c = 0; c < spec->columns.size(); c++) {
				const auto &col = spec->columns[c].name;
				if (Ignored(col) || o[c] == n[c]) {
					continue;
				}
				ChangeRow row = base(k, n, "modified");
				row.field = col;
				row.old_value = o[c];
				row.new_value = n[c];
				out.push_back(std::move(row));
			}
		}
		for (auto &k : old_keyed) {
			if (!matched.count(k.key)) {
				out.push_back(base(k, *k.row, "removed"));
			}
		}
	}
}

} // namespace orcid
