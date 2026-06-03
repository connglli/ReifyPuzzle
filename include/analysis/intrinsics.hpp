#pragma once

#include <optional>
#include <string>

namespace symir {

  /**
   * Represents the kind of standard built-in intrinsic.
   * Standard built-in intrinsics (§12) require no user-supplied body or contracts.
   * Semantics are hardcoded inside the interpreter, solver, and codegen backends.
   *
   * v0.2.2 baseline (§12.1, §12.2): Abs, Min, Max, Popcount, Clz, Ctz.
   * v0.2.2 extra A/B (§12.3 integer extras, §12.4 bit-manipulation):
   *   AbsDiff, Signum, Clamp, Midpoint,
   *   Parity, Bswap, Bitreverse, Rotl, Rotr, IsPow2, Ilog2.
   * v0.2.2 extra C — integer overflow-aware family (§12.5):
   *   WrappingAdd, WrappingSub, WrappingMul, WrappingNeg,
   *   WrappingShl, WrappingShr,
   *   SaturatingAdd, SaturatingSub, SaturatingMul, SaturatingNeg,
   *   DivEuclid, RemEuclid.
   * v0.2.2 extra D.1 — floating-point sign / bit ops (§12.6):
   *   Fabs, Fneg, Copysign, Signbit, ToBits, FromBits.
   * v0.2.2 extra D.2 — floating-point classification predicates (§12.6):
   *   IsNormal, IsSubnormal.
   *
   * Note: there is no @umin / @umax — SymIR has no unsigned integer types
   * and the toolchain does not implicitly reinterpret iN bits as uN.
   * The tuple-returning members of the overflow family (`@checked_*`,
   * `@overflowing_*`) and the cross-width `@widening_mul` are deferred
   * to a follow-up batch that introduces the multi-value return ABI.
   */
  enum class IntrinsicKind {
    // v0.2.2 baseline
    Abs,
    Min,
    Max,
    Popcount,
    Clz,
    Ctz,
    // v0.2.2 extra batch A — integer extras (§12.3)
    AbsDiff,
    Signum,
    Clamp,
    Midpoint,
    // v0.2.2 extra batch B — bit-manipulation (§12.4)
    Parity,
    Bswap,
    Bitreverse,
    Rotl,
    Rotr,
    IsPow2,
    Ilog2,
    // v0.2.2 extra batch C — integer overflow-aware family (§12.5)
    WrappingAdd,
    WrappingSub,
    WrappingMul,
    WrappingNeg,
    WrappingShl,
    WrappingShr,
    SaturatingAdd,
    SaturatingSub,
    SaturatingMul,
    SaturatingNeg,
    DivEuclid,
    RemEuclid,
    // v0.2.2 extra batch D.1 — floating-point sign / bit ops (§12.6)
    Fabs,
    Fneg,
    Copysign,
    Signbit,
    ToBits,
    FromBits,
    // v0.2.2 extra batch D.2 — floating-point classification predicates (§12.6)
    IsNormal,
    IsSubnormal,
  };

  /**
   * Resolves a global identifier string (such as "@abs") to its corresponding IntrinsicKind.
   * Returns std::nullopt if the name does not match any recognized built-in intrinsic.
   */
  inline std::optional<IntrinsicKind> getIntrinsicKind(const std::string &name) {
    // v0.2.2 basline
    if (name == "@abs")
      return IntrinsicKind::Abs;
    if (name == "@min")
      return IntrinsicKind::Min;
    if (name == "@max")
      return IntrinsicKind::Max;
    if (name == "@popcount")
      return IntrinsicKind::Popcount;
    if (name == "@clz")
      return IntrinsicKind::Clz;
    if (name == "@ctz")
      return IntrinsicKind::Ctz;
    // v0.2.2 extra batch A — integer extras (§12.3)
    if (name == "@abs_diff")
      return IntrinsicKind::AbsDiff;
    if (name == "@signum")
      return IntrinsicKind::Signum;
    if (name == "@clamp")
      return IntrinsicKind::Clamp;
    if (name == "@midpoint")
      return IntrinsicKind::Midpoint;
    // v0.2.2 extra batch B — bit-manipulation (§12.4)
    if (name == "@parity")
      return IntrinsicKind::Parity;
    if (name == "@bswap")
      return IntrinsicKind::Bswap;
    if (name == "@bitreverse")
      return IntrinsicKind::Bitreverse;
    if (name == "@rotl")
      return IntrinsicKind::Rotl;
    if (name == "@rotr")
      return IntrinsicKind::Rotr;
    if (name == "@is_pow2")
      return IntrinsicKind::IsPow2;
    if (name == "@ilog2")
      return IntrinsicKind::Ilog2;
    // v0.2.2 extra batch C — integer overflow-aware family (§12.5)
    if (name == "@wrapping_add")
      return IntrinsicKind::WrappingAdd;
    if (name == "@wrapping_sub")
      return IntrinsicKind::WrappingSub;
    if (name == "@wrapping_mul")
      return IntrinsicKind::WrappingMul;
    if (name == "@wrapping_neg")
      return IntrinsicKind::WrappingNeg;
    if (name == "@wrapping_shl")
      return IntrinsicKind::WrappingShl;
    if (name == "@wrapping_shr")
      return IntrinsicKind::WrappingShr;
    if (name == "@saturating_add")
      return IntrinsicKind::SaturatingAdd;
    if (name == "@saturating_sub")
      return IntrinsicKind::SaturatingSub;
    if (name == "@saturating_mul")
      return IntrinsicKind::SaturatingMul;
    if (name == "@saturating_neg")
      return IntrinsicKind::SaturatingNeg;
    if (name == "@div_euclid")
      return IntrinsicKind::DivEuclid;
    if (name == "@rem_euclid")
      return IntrinsicKind::RemEuclid;
    // v0.2.2 extra batch D.1 — floating-point sign / bit ops (§12.6)
    if (name == "@fabs")
      return IntrinsicKind::Fabs;
    if (name == "@fneg")
      return IntrinsicKind::Fneg;
    if (name == "@copysign")
      return IntrinsicKind::Copysign;
    if (name == "@signbit")
      return IntrinsicKind::Signbit;
    if (name == "@to_bits")
      return IntrinsicKind::ToBits;
    if (name == "@from_bits")
      return IntrinsicKind::FromBits;
    // v0.2.2 extra batch D.2 — floating-point classification predicates (§12.6)
    if (name == "@is_normal")
      return IntrinsicKind::IsNormal;
    if (name == "@is_subnormal")
      return IntrinsicKind::IsSubnormal;
    return std::nullopt;
  }

} // namespace symir
