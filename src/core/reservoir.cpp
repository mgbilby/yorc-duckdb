#include "orcid/reservoir.hpp"

#include "orcid/id.hpp"

namespace orcid {

uint64_t MulHigh64(uint64_t a, uint64_t b) {
	const uint64_t mask = 0xFFFFFFFFull;
	const uint64_t a_lo = a & mask, a_hi = a >> 32;
	const uint64_t b_lo = b & mask, b_hi = b >> 32;
	const uint64_t p0 = a_lo * b_lo;
	const uint64_t p1 = a_lo * b_hi;
	const uint64_t p2 = a_hi * b_lo;
	const uint64_t p3 = a_hi * b_hi;
	// carry out of the low 64 bits: at most 2 + 2 * (2^32 - 1) < 2^34, no overflow
	const uint64_t mid = (p0 >> 32) + (p1 & mask) + (p2 & mask);
	return p3 + (p1 >> 32) + (p2 >> 32) + (mid >> 32);
}

uint64_t SplitMix64::Next() {
	state_ += 0x9E3779B97F4A7C15ull; // unsigned: wraps, as Rust's wrapping_add
	uint64_t z = state_;
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
	return z ^ (z >> 31);
}

uint64_t SplitMix64::Below(uint64_t n) {
	return MulHigh64(Next(), n);
}

Slot ReservoirSampler::Offer() {
	const uint64_t k = seen_++;
	Slot slot;
	if (capacity_ == 0) {
		return slot; // Skip, no draw
	}
	if (k < capacity_) {
		slot.kind = SlotKind::Append;
		return slot;
	}
	const uint64_t j = rng_.Below(k + 1);
	if (j < capacity_) {
		slot.kind = SlotKind::Replace;
		slot.index = j;
	}
	return slot;
}

bool DumpSampleEligible(const std::string &member_path, std::string &orcid) {
	const size_t slash = member_path.find_last_of('/');
	const size_t start = slash == std::string::npos ? 0 : slash + 1;
	const size_t len = member_path.size() - start;
	if (len <= 4) {
		return false;
	}
	static const char suffix[] = ".xml";
	for (size_t i = 0; i < 4; i++) {
		char c = member_path[member_path.size() - 4 + i];
		if (c >= 'A' && c <= 'Z') {
			c = static_cast<char>(c - 'A' + 'a');
		}
		if (c != suffix[i]) {
			return false;
		}
	}
	return NormalizeId(member_path.substr(start, len - 4), orcid);
}

} // namespace orcid
