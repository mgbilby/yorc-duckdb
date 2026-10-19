#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>

namespace orcid {

class TarError : public std::runtime_error {
public:
	using std::runtime_error::runtime_error;
};

//! One regular-file member of a tar archive.
struct TarMember {
	std::string name;
	uint64_t size = 0;
};

//! Streaming reader for POSIX ustar, GNU (long names, 'L') and pax ('x' path)
//! archives, as written by GNU tar for the ORCID Public Data File. Reads
//! strictly forward through `read`, so it works on a gzip stream or an HTTP
//! body without seeking; stopping early stops reading.
class TarReader {
public:
	//! Fills up to `n` bytes, returns how many were read; 0 means end of input.
	using ReadFn = std::function<size_t(char *buf, size_t n)>;

	explicit TarReader(ReadFn read) : read_(std::move(read)) {
	}

	//! Advances to the next regular file (directories, links and other entry
	//! types are skipped). Returns false at the end of the archive. Skips any
	//! unread content of the previous member. Throws TarError on a corrupt or
	//! truncated archive.
	bool Next(TarMember &member);
	//! The current member's content. Call at most once per member.
	std::string ReadContent();

private:
	bool ReadBlock(char *block);              // false only at a clean end of input
	void ReadExact(char *buf, size_t n);      // throws on short read
	void Skip(uint64_t n);
	std::string ReadString(uint64_t n);       // n bytes, memory grown as they arrive

	ReadFn read_;
	uint64_t remaining_ = 0; // unread content bytes of the current member
	uint64_t padding_ = 0;   // padding after the current member's content
};

//! Shell-style match of a member path: '*' matches any run of characters
//! (including '/'), '?' one character. Used for the `members` filter.
bool GlobMatch(const std::string &pattern, const std::string &text);

} // namespace orcid
