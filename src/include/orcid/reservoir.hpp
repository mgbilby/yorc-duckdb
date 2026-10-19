#pragma once

#include <cstdint>
#include <string>

namespace orcid {

//! SplitMix64 (Steele, Lea and Flood 2014), exactly as yorc-rs's
//! `dump::reservoir::SplitMix64` and yorc-zig's `reservoir.SplitMix64`, so that
//! the three packages draw the same numbers from the same seed.
class SplitMix64 {
public:
	explicit SplitMix64(uint64_t seed) : state_(seed) {
	}
	//! The next 64-bit output.
	uint64_t Next();
	//! A value in [0, n); n must be > 0. Lemire's multiply-shift
	//! `(Next() * n) >> 64` on the full 128-bit product, with no rejection step
	//! (the rule yorc-rs and yorc-zig specify).
	uint64_t Below(uint64_t n);

private:
	uint64_t state_;
};

//! The high 64 bits of the 128-bit product a * b (portable: no __int128).
uint64_t MulHigh64(uint64_t a, uint64_t b);

//! What to do with the eligible item just offered (yorc-rs's `Slot`).
enum class SlotKind { Append, Replace, Skip };

struct Slot {
	SlotKind kind = SlotKind::Skip;
	uint64_t index = 0; //!< the slot replaced, for SlotKind::Replace
};

//! Vitter's Algorithm R, seeded, exactly as yorc-rs's `dump::Reservoir`: the
//! k-th eligible item (0-based) is kept unconditionally while k < capacity;
//! otherwise one draw j = Below(k + 1) is made and the item replaces slot j
//! iff j < capacity. With capacity 0 nothing is kept and nothing is drawn.
//! Only the decision is made here; the caller stores the items, so that an
//! item that is skipped is never read.
class ReservoirSampler {
public:
	ReservoirSampler(uint64_t capacity, uint64_t seed) : capacity_(capacity), rng_(seed) {
	}
	//! Call exactly once per eligible item, in stream order.
	Slot Offer();
	//! Eligible items offered so far.
	uint64_t Seen() const {
		return seen_;
	}

private:
	uint64_t capacity_;
	uint64_t seen_ = 0;
	SplitMix64 rng_;
};

//! Whether an archive member (a regular file, by its path in the archive) is
//! offered to the sampler, as in the Rust and Zig walkthroughs (cell 8) and
//! bench programs: its last path component (after the last '/') is longer than
//! four characters and ends in ".xml" (any case), and what comes before is an
//! ORCID iD by NormalizeId (check character verified). On true, `orcid` is the
//! canonical iD.
bool DumpSampleEligible(const std::string &member_path, std::string &orcid);

} // namespace orcid
