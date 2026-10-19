// libFuzzer target: DiffDocuments over two documents. The input is split at
// the first NUL byte (old \0 new); without one, both sides are the input. Each
// side's format (XML or JSON) is told by orcid::DetectFormat, as the
// orcid_changes file reader does.
#include "orcid/changes.hpp"
#include "orcid/json.hpp"
#include "orcid/json_map.hpp"
#include "orcid/xml.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	const char *p = reinterpret_cast<const char *>(data);
	const void *nul = std::memchr(p, '\0', size);
	size_t old_len = nul ? static_cast<size_t>(static_cast<const char *>(nul) - p) : size;
	const char *new_doc = nul ? p + old_len + 1 : p;
	size_t new_len = nul ? size - old_len - 1 : size;
	std::vector<orcid::ChangeRow> out;
	try {
		orcid::DiffDocuments(p, old_len, orcid::DetectFormat(p, old_len), new_doc, new_len,
		                     orcid::DetectFormat(new_doc, new_len), std::string(), out);
	} catch (const orcid::XmlError &) {
	} catch (const orcid::JsonError &) {
	} catch (const orcid::JsonRootError &) {
	}
	return 0;
}
