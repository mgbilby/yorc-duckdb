#include "orcid/id.hpp"

#include <cctype>
#include <initializer_list>

namespace orcid {

static bool StartsWith(const std::string &s, size_t pos, const char *prefix) {
	return s.compare(pos, std::char_traits<char>::length(prefix), prefix) == 0;
}

bool NormalizeId(const std::string &input, std::string &out) {
	size_t pos = 0;
	while (pos < input.size() && std::isspace(static_cast<unsigned char>(input[pos]))) {
		pos++;
	}
	for (const char *prefix : {"https://orcid.org/", "http://orcid.org/", "orcid.org/"}) {
		if (StartsWith(input, pos, prefix)) {
			pos += std::char_traits<char>::length(prefix);
			break;
		}
	}

	char chars[16];
	size_t n = 0;
	for (; pos < input.size(); pos++) {
		auto c = static_cast<unsigned char>(input[pos]);
		if (c == '-' || std::isspace(c)) {
			continue;
		}
		if (n == 16) {
			return false;
		}
		chars[n++] = static_cast<char>(std::toupper(c));
	}
	if (n != 16) {
		return false;
	}

	unsigned total = 0;
	for (size_t i = 0; i < 15; i++) {
		if (chars[i] < '0' || chars[i] > '9') {
			return false;
		}
		total = (total + static_cast<unsigned>(chars[i] - '0')) * 2;
	}
	unsigned digit = (12 - total % 11) % 11;
	char check = digit == 10 ? 'X' : static_cast<char>('0' + digit);
	if (chars[15] != check) {
		return false;
	}

	out.clear();
	out.reserve(19);
	for (size_t i = 0; i < 16; i++) {
		if (i > 0 && i % 4 == 0) {
			out.push_back('-');
		}
		out.push_back(chars[i]);
	}
	return true;
}

bool IsValidId(const std::string &input) {
	std::string unused;
	return NormalizeId(input, unused);
}

} // namespace orcid
