#pragma once

#include <set>
#include <string>
#include <utility>
#include <vector>
#include "analysis/intrinsics.hpp"
#include "ast/ast.hpp"

namespace refractir::reify {

  /**
   * Metadata for a solver-friendly intrinsic that rysmith is allowed to
   * generate. One entry fully describes an intrinsic's shape:
   *
   *   - `vectorParam` — the parameter(s) are `<N> T` (the horizontal
   *     reductions, §12.4) rather than scalar `T`. The return is scalar `T`
   *     either way.
   *   - `allowsFloat` — the element type `T` may be a floating-point `fN`;
   *     otherwise the intrinsic is integer-only.
   *
   * Reductions are just intrinsics with `vectorParam = true`; nothing else
   * about selection, use-tracking or declaration treats them specially.
   */
  struct WhitelistedIntrinsic {
    IntrinsicKind kind;
    const char *name; // "@abs", "@reduce_add", etc.
    int paramCount;   // number of parameters
    bool returnsI1;   // predicate result (@parity, @is_pow2) — not a target
    bool vectorParam; // parameter(s) are <N> T (reductions) vs scalar T
    bool allowsFloat; // element T may be fN (else iN only)
  };

  inline const std::vector<WhitelistedIntrinsic> &getIntrinsicWhitelist() {
    // Fields: kind, name, paramCount, returnsI1, vectorParam, allowsFloat.
    static const std::vector<WhitelistedIntrinsic> list = {
        // v0.2.2 baseline (§12.1, §12.2)
        {IntrinsicKind::Abs, "@abs", 1, false, false, false},
        {IntrinsicKind::Min, "@min", 2, false, false, false},
        {IntrinsicKind::Max, "@max", 2, false, false, false},
        {IntrinsicKind::Popcount, "@popcount", 1, false, false, false},
        {IntrinsicKind::Clz, "@clz", 1, false, false, false},
        {IntrinsicKind::Ctz, "@ctz", 1, false, false, false},
        // v0.2.2 extra batch A — integer extras (§12.3)
        {IntrinsicKind::AbsDiff, "@abs_diff", 2, false, false, false},
        {IntrinsicKind::Signum, "@signum", 1, false, false, false},
        {IntrinsicKind::Clamp, "@clamp", 3, false, false, false},
        {IntrinsicKind::Midpoint, "@midpoint", 2, false, false, false},
        // v0.2.2 extra batch B — bit-manipulation (§12.4)
        {IntrinsicKind::Parity, "@parity", 1, true, false, false},
        {IntrinsicKind::Bswap, "@bswap", 1, false, false, false},
        {IntrinsicKind::Bitreverse, "@bitreverse", 1, false, false, false},
        {IntrinsicKind::Rotl, "@rotl", 2, false, false, false},
        {IntrinsicKind::Rotr, "@rotr", 2, false, false, false},
        {IntrinsicKind::IsPow2, "@is_pow2", 1, true, false, false},
        {IntrinsicKind::Ilog2, "@ilog2", 1, false, false, false},
        // v0.2.2 extra batch C — integer overflow-aware family (§12.5)
        {IntrinsicKind::WrappingAdd, "@wrapping_add", 2, false, false, false},
        {IntrinsicKind::WrappingSub, "@wrapping_sub", 2, false, false, false},
        {IntrinsicKind::WrappingMul, "@wrapping_mul", 2, false, false, false},
        {IntrinsicKind::WrappingNeg, "@wrapping_neg", 1, false, false, false},
        {IntrinsicKind::WrappingShl, "@wrapping_shl", 2, false, false, false},
        {IntrinsicKind::WrappingShr, "@wrapping_shr", 2, false, false, false},
        {IntrinsicKind::SaturatingAdd, "@saturating_add", 2, false, false, false},
        {IntrinsicKind::SaturatingSub, "@saturating_sub", 2, false, false, false},
        {IntrinsicKind::SaturatingMul, "@saturating_mul", 2, false, false, false},
        {IntrinsicKind::SaturatingNeg, "@saturating_neg", 1, false, false, false},
        {IntrinsicKind::DivEuclid, "@div_euclid", 2, false, false, false},
        {IntrinsicKind::RemEuclid, "@rem_euclid", 2, false, false, false},
        // v0.2.3 horizontal reductions (§12.4) — vector parameter, scalar
        // result. add/min/max fold integer or FP lanes; the bitwise ones
        // are integer-only.
        {IntrinsicKind::ReduceAdd, "@reduce_add", 1, false, true, true},
        {IntrinsicKind::ReduceMin, "@reduce_min", 1, false, true, true},
        {IntrinsicKind::ReduceMax, "@reduce_max", 1, false, true, true},
        {IntrinsicKind::ReduceAnd, "@reduce_and", 1, false, true, false},
        {IntrinsicKind::ReduceOr, "@reduce_or", 1, false, true, false},
        {IntrinsicKind::ReduceXor, "@reduce_xor", 1, false, true, false},
    };
    return list;
  }

  // Append one IntrinsicDecl per used instantiation. This is the single
  // place a whitelist entry becomes a declaration; func_gen and twin_gen
  // both emit their `intrinsic` sections through it. The return is always
  // scalar `T`; the only per-intrinsic variation is whether the parameter
  // type is `<N> T` (reductions) or scalar `T`.
  inline void
  appendUsedIntrinsicDecls(const std::set<IntrinsicUseKey> &used, std::vector<IntrinsicDecl> &out) {
    auto makeScalarTy = [](uint32_t bits, bool isFloat) -> TypePtr {
      if (isFloat)
        return std::make_shared<Type>(
            Type{FloatType{bits == 32 ? FloatType::Kind::F32 : FloatType::Kind::F64, {}}, {}}
        );
      if (bits == 32)
        return std::make_shared<Type>(Type{IntType{IntType::Kind::I32, {}, {}}, {}});
      if (bits == 64)
        return std::make_shared<Type>(Type{IntType{IntType::Kind::I64, {}, {}}, {}});
      return std::make_shared<Type>(Type{IntType{IntType::Kind::ICustom, (int) bits, {}}, {}});
    };
    for (const auto &key: used) {
      const WhitelistedIntrinsic *wi = nullptr;
      for (const auto &w: getIntrinsicWhitelist())
        if (w.kind == key.kind) {
          wi = &w;
          break;
        }
      if (!wi)
        continue;
      TypePtr scalarTy = makeScalarTy(key.elemBits, key.elemIsFloat);
      TypePtr paramTy = wi->vectorParam
                            ? std::make_shared<Type>(Type{VecType{key.lanes, scalarTy, {}}, {}})
                            : scalarTy;
      IntrinsicDecl id;
      id.name = GlobalId{std::string(wi->name), {}};
      id.retType = scalarTy;
      for (int pi = 0; pi < wi->paramCount; pi++) {
        ParamDecl pd;
        pd.name = LocalId{"%x" + std::to_string(pi), {}};
        pd.type = paramTy;
        id.params.push_back(std::move(pd));
      }
      out.push_back(std::move(id));
    }
  }

} // namespace refractir::reify
