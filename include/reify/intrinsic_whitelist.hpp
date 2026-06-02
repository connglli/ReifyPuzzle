#pragma once

#include <vector>
#include "analysis/intrinsics.hpp"

namespace symir::reify {

  /**
   * Metadata for a solver-friendly intrinsic that rysmith is allowed to generate.
   *
   * All 29 intrinsics shipped in v0.2.2 (baseline + extra batches A/B/C) are P0
   * (solver-friendly) — they decompose into quantifier-free BV operations with
   * bounded O(N) ITE chains.
   */
  struct WhitelistedIntrinsic {
    IntrinsicKind kind;
    const char *name; // "@abs", "@clamp", etc.
    int paramCount;   // 1, 2, or 3
    bool returnsI1;   // true for @parity, @is_pow2
  };

  inline const std::vector<WhitelistedIntrinsic> &getIntrinsicWhitelist() {
    static const std::vector<WhitelistedIntrinsic> list = {
        // v0.2.2 baseline (§12.1, §12.2)
        {IntrinsicKind::Abs, "@abs", 1, false},
        {IntrinsicKind::Min, "@min", 2, false},
        {IntrinsicKind::Max, "@max", 2, false},
        {IntrinsicKind::Popcount, "@popcount", 1, false},
        {IntrinsicKind::Clz, "@clz", 1, false},
        {IntrinsicKind::Ctz, "@ctz", 1, false},
        // v0.2.2 extra batch A — integer extras (§12.3)
        {IntrinsicKind::AbsDiff, "@abs_diff", 2, false},
        {IntrinsicKind::Signum, "@signum", 1, false},
        {IntrinsicKind::Clamp, "@clamp", 3, false},
        {IntrinsicKind::Midpoint, "@midpoint", 2, false},
        // v0.2.2 extra batch B — bit-manipulation (§12.4)
        {IntrinsicKind::Parity, "@parity", 1, true},
        {IntrinsicKind::Bswap, "@bswap", 1, false},
        {IntrinsicKind::Bitreverse, "@bitreverse", 1, false},
        {IntrinsicKind::Rotl, "@rotl", 2, false},
        {IntrinsicKind::Rotr, "@rotr", 2, false},
        {IntrinsicKind::IsPow2, "@is_pow2", 1, true},
        {IntrinsicKind::Ilog2, "@ilog2", 1, false},
        // v0.2.2 extra batch C — integer overflow-aware family (§12.5)
        {IntrinsicKind::WrappingAdd, "@wrapping_add", 2, false},
        {IntrinsicKind::WrappingSub, "@wrapping_sub", 2, false},
        {IntrinsicKind::WrappingMul, "@wrapping_mul", 2, false},
        {IntrinsicKind::WrappingNeg, "@wrapping_neg", 1, false},
        {IntrinsicKind::WrappingShl, "@wrapping_shl", 2, false},
        {IntrinsicKind::WrappingShr, "@wrapping_shr", 2, false},
        {IntrinsicKind::SaturatingAdd, "@saturating_add", 2, false},
        {IntrinsicKind::SaturatingSub, "@saturating_sub", 2, false},
        {IntrinsicKind::SaturatingMul, "@saturating_mul", 2, false},
        {IntrinsicKind::SaturatingNeg, "@saturating_neg", 1, false},
        {IntrinsicKind::DivEuclid, "@div_euclid", 2, false},
        {IntrinsicKind::RemEuclid, "@rem_euclid", 2, false},
    };
    return list;
  }

} // namespace symir::reify
