#pragma once

#include <string>

namespace orcid {

// Canonicalizes an ORCID iD to XXXX-XXXX-XXXX-XXXX. Accepts orcid.org URL
// prefixes, whitespace, missing hyphens and a lowercase check character.
// Returns false if the input is not 16 characters or fails ISO 7064 MOD 11-2.
bool NormalizeId(const std::string &input, std::string &out);

bool IsValidId(const std::string &input);

} // namespace orcid
