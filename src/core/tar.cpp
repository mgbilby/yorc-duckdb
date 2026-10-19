#include "orcid/tar.hpp"

#include <cstdlib>
#include <cstring>
#include <vector>

namespace orcid {

namespace {

constexpr size_t BLOCK = 512;
//! Largest GNU long-name / pax extended header accepted (real ones are < 1 KiB).
constexpr uint64_t MAX_HEADER_DATA = 1 << 20;
//! Content is read in pieces of this size, so memory follows the bytes that
//! actually arrive, not the size a (possibly corrupt) header claims.
constexpr size_t READ_PIECE = 1 << 20;

std::string Field(const char *p, size_t n) {
	size_t len = 0;
	while (len < n && p[len] != '\0') {
		len++;
	}
	return std::string(p, len);
}

//! Octal numeric field, or GNU base-256 when the high bit of the first byte is set.
uint64_t Number(const char *p, size_t n) {
	auto u = reinterpret_cast<const unsigned char *>(p);
	if (n > 0 && (u[0] & 0x80)) {
		uint64_t v = u[0] & 0x7f;
		for (size_t i = 1; i < n; i++) {
			v = (v << 8) | u[i];
		}
		return v;
	}
	uint64_t v = 0;
	size_t i = 0;
	while (i < n && (p[i] == ' ' || p[i] == '\0')) {
		i++;
	}
	for (; i < n && p[i] >= '0' && p[i] <= '7'; i++) {
		v = (v << 3) | static_cast<uint64_t>(p[i] - '0');
	}
	return v;
}

bool IsZeroBlock(const char *block) {
	for (size_t i = 0; i < BLOCK; i++) {
		if (block[i] != '\0') {
			return false;
		}
	}
	return true;
}

bool ChecksumOk(const char *block) {
	uint64_t stored = Number(block + 148, 8);
	uint64_t sum = 0;
	for (size_t i = 0; i < BLOCK; i++) {
		sum += (i >= 148 && i < 156) ? ' ' : static_cast<unsigned char>(block[i]);
	}
	return sum == stored;
}

//! The "path" record of a pax extended header ("<len> path=<value>\n" ...).
std::string PaxPath(const std::string &data) {
	size_t pos = 0;
	while (pos < data.size()) {
		size_t space = data.find(' ', pos);
		if (space == std::string::npos) {
			break;
		}
		size_t len = std::strtoul(data.c_str() + pos, nullptr, 10);
		if (len == 0 || len > data.size() - pos || space >= pos + len) {
			break;
		}
		std::string record = data.substr(space + 1, pos + len - space - 2); // drop trailing '\n'
		if (record.compare(0, 5, "path=") == 0) {
			return record.substr(5);
		}
		pos += len;
	}
	return std::string();
}

} // namespace

void TarReader::ReadExact(char *buf, size_t n) {
	size_t got = 0;
	while (got < n) {
		size_t r = read_(buf + got, n - got);
		if (r == 0) {
			throw TarError("truncated tar archive");
		}
		got += r;
	}
}

bool TarReader::ReadBlock(char *block) {
	size_t got = 0;
	while (got < BLOCK) {
		size_t r = read_(block + got, BLOCK - got);
		if (r == 0) {
			if (got == 0) {
				return false;
			}
			throw TarError("truncated tar header");
		}
		got += r;
	}
	return true;
}

std::string TarReader::ReadString(uint64_t n) {
	std::string out;
	while (n > 0) {
		size_t piece = n < READ_PIECE ? static_cast<size_t>(n) : READ_PIECE;
		size_t at = out.size();
		out.resize(at + piece);
		ReadExact(&out[at], piece);
		n -= piece;
	}
	return out;
}

void TarReader::Skip(uint64_t n) {
	char buf[64 * 1024];
	while (n > 0) {
		size_t chunk = n < sizeof(buf) ? static_cast<size_t>(n) : sizeof(buf);
		ReadExact(buf, chunk);
		n -= chunk;
	}
}

bool TarReader::Next(TarMember &member) {
	Skip(remaining_);
	remaining_ = 0;
	Skip(padding_);
	padding_ = 0;

	std::string long_name;
	char block[BLOCK];
	while (true) {
		if (!ReadBlock(block) || IsZeroBlock(block)) {
			return false; // end-of-archive marker or end of input
		}
		if (!ChecksumOk(block)) {
			throw TarError("bad tar header checksum (not a tar archive, or corrupt)");
		}
		const char type = block[156];
		const uint64_t size = Number(block + 124, 12);
		const uint64_t pad = (BLOCK - size % BLOCK) % BLOCK;

		if (type == 'L' || type == 'x') { // GNU long name / pax extended header
			if (size > MAX_HEADER_DATA) {
				throw TarError("oversized tar extended header");
			}
			std::string data = ReadString(size);
			Skip(pad);
			std::string name = type == 'L' ? Field(data.data(), data.size()) : PaxPath(data);
			if (!name.empty()) {
				long_name = name;
			}
			continue;
		}
		if (type != '0' && type != '\0' && type != '7') { // not a regular file
			Skip(size);
			Skip(pad);
			long_name.clear();
			continue;
		}
		if (!long_name.empty()) {
			member.name = long_name;
		} else {
			std::string prefix = Field(block + 345, 155);
			std::string name = Field(block, 100);
			member.name = prefix.empty() || std::memcmp(block + 257, "ustar", 5) != 0 ? name : prefix + "/" + name;
		}
		member.size = size;
		remaining_ = size;
		padding_ = pad;
		return true;
	}
}

std::string TarReader::ReadContent() {
	std::string content = ReadString(remaining_);
	remaining_ = 0;
	return content;
}

bool GlobMatch(const std::string &pattern, const std::string &text) {
	size_t p = 0, t = 0, star = std::string::npos, mark = 0;
	while (t < text.size()) {
		if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == text[t])) {
			p++;
			t++;
		} else if (p < pattern.size() && pattern[p] == '*') {
			star = p++;
			mark = t;
		} else if (star != std::string::npos) {
			p = star + 1;
			t = ++mark;
		} else {
			return false;
		}
	}
	while (p < pattern.size() && pattern[p] == '*') {
		p++;
	}
	return p == pattern.size();
}

} // namespace orcid
