// libFuzzer target: the streaming tar reader (ustar, GNU long names, pax) and
// the member glob, over an archive held in memory and fed in odd-sized reads.
#include "orcid/tar.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	size_t pos = 0;
	size_t step = size > 0 ? 1 + data[0] % 7 * 97 : 1; // vary the read sizes
	orcid::TarReader tar([&](char *buf, size_t n) -> size_t {
		size_t k = std::min({n, size - pos, step});
		std::memcpy(buf, data + pos, k);
		pos += k;
		return k;
	});
	try {
		orcid::TarMember member;
		for (int i = 0; tar.Next(member); i++) {
			(void)orcid::GlobMatch("*.xml", member.name);
			(void)orcid::GlobMatch(member.name, "summaries/097/0000-0002-1825-0097.xml");
			if (i % 2 == 0) {
				std::string content = tar.ReadContent();
				if (content.size() != member.size) {
					__builtin_trap();
				}
			}
		}
	} catch (const orcid::TarError &) {
	}
	return 0;
}
