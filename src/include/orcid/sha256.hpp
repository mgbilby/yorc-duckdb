#pragma once

#include <cstddef>
#include <string>

namespace orcid {

// Lowercase hex SHA-256 (FIPS 180-4) of `len` bytes at `data`.
std::string Sha256Hex(const char *data, size_t len);

} // namespace orcid
