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
   *
   * Note: there is no @umin / @umax — SymIR has no unsigned integer types
   * and the toolchain does not implicitly reinterpret iN bits as uN.
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
    return std::nullopt;
  }

} // namespace symir
