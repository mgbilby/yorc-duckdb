// libFuzzer target: one JSON document through the JSON parser, every section
// extractor (JSON mapped onto the XML element tree) and the writers.
// Build and run: `make fuzz` (see test/fuzz/README.md).
// Written without running.
//
// Properties checked, beyond "no crash, no sanitizer report":
// - a parsed string re-escaped with AppendJsonString parses back to itself;
// - every works row the extractor yields, written back with the writers,
//   gives an XML and a JSON document that parse (or the writer refuses the
//   row with WriteError, e.g. a year outside 1900..2100).
#include "orcid/json.hpp"
#include "orcid/json_map.hpp"
#include "orcid/sections.hpp"
#include "orcid/writer.hpp"
#include "orcid/xml.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <string>
#include <vector>

static void CheckStrings(const orcid::JsonValue &v) {
	if (v.type == orcid::JsonValue::Type::String) {
		std::string literal;
		orcid::AppendJsonString(v.text, literal); // parsed text is valid UTF-8, so this cannot throw
		if (orcid::ParseJson(literal.data(), literal.size()).text != v.text) {
			std::abort();
		}
	}
	for (auto &item : v.items) {
		CheckStrings(item);
	}
	for (auto &m : v.members) {
		CheckStrings(m.second);
	}
}

static void ParsesBack(const std::string &doc, orcid::WriteFormat format) {
	const orcid::SectionSpec *works = orcid::FindSection("works");
	std::vector<orcid::Row> rows;
	try {
		orcid::ExtractDocument(*works, doc.data(), doc.size(),
		                       format == orcid::WriteFormat::Json ? orcid::DocFormat::Json : orcid::DocFormat::Xml,
		                       std::string(), rows);
	} catch (const std::exception &) {
		std::abort(); // the writers' own output must always read back
	}
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	const char *json = reinterpret_cast<const char *>(data);
	try {
		CheckStrings(orcid::ParseJson(json, size));
	} catch (const orcid::JsonError &) {
		return 0; // malformed input is reported, never a crash
	}
	for (auto &spec : orcid::Sections()) {
		std::vector<orcid::Row> rows;
		try {
			orcid::ExtractDocument(spec, json, size, orcid::DocFormat::Json, std::string(), rows);
		} catch (const orcid::JsonRootError &) {
			continue;
		} catch (const orcid::XmlError &) {
			continue;
		}
		for (auto &row : rows) {
			if (row.size() != spec.columns.size()) {
				std::abort();
			}
		}
		if (spec.name != "works") {
			continue;
		}
		for (auto &row : rows) {
			orcid::WriteRecord record;
			for (size_t c = 0; c < row.size(); c++) {
				if (row[c]) {
					record.fields[spec.columns[c].name] = *row[c];
				}
			}
			for (auto format : {orcid::WriteFormat::Xml, orcid::WriteFormat::Json}) {
				std::string doc;
				try {
					doc = orcid::WriteDocument("work", record, format);
				} catch (const orcid::WriteError &) {
					continue;
				}
				ParsesBack(doc, format);
			}
		}
	}
	return 0;
}
