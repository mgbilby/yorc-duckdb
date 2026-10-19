// libFuzzer target: every section extractor over one XML document.
// Build and run: `make fuzz` (see test/fuzz/README.md).
#include "orcid/sections.hpp"
#include "orcid/xml.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	const char *xml = reinterpret_cast<const char *>(data);
	for (auto &spec : orcid::Sections()) {
		std::vector<orcid::Row> rows;
		try {
			orcid::ExtractSection(spec, xml, size, rows);
		} catch (const orcid::XmlError &) {
			continue; // malformed input is reported, never a crash
		}
		for (auto &row : rows) {
			if (row.size() != spec.columns.size()) {
				std::abort();
			}
			// what the DuckDB binding does with every value
			for (size_t c = 0; c < row.size(); c++) {
				if (!row[c]) {
					continue;
				}
				switch (spec.columns[c].type) {
				case orcid::ColType::Integer:
					(void)orcid::ParseInteger(*row[c], 9);
					break;
				case orcid::ColType::Bigint:
					(void)orcid::ParseInteger(*row[c], 18);
					break;
				case orcid::ColType::Boolean:
					(void)orcid::ParseBoolean(*row[c]);
					break;
				default:
					break;
				}
			}
		}
	}
	return 0;
}
