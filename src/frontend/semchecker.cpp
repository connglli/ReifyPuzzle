#include "frontend/semchecker.hpp"
#include <algorithm>
#include "analysis/intrinsics.hpp"
#include "analysis/type_utils.hpp"

namespace refractir {

  refractir::PassResult SemChecker::run(Program &prog, DiagBag &diags) {
    std::unordered_set<std::string> globalNames;

    for (const auto &s: prog.structs) {
      if (globalNames.count(s.name.name)) {
        diags.error("Duplicate global name (struct): " + s.name.name, s.span);
      }
      globalNames.insert(s.name.name);
      checkStruct(s, diags);
    }

    for (const auto &f: prog.funs) {
      if (globalNames.count(f.name.name)) {
        diags.error("Duplicate global name (function): " + f.name.name, f.span);
      }
      globalNames.insert(f.name.name);
      checkFunction(f, diags);
    }

    // [v0.2.2] External declarations: each `decl @name` is a global name and
    // must not collide with any struct/fun/intrinsic. A contract-form `decl`
    // and a `fun` with the same name within the same file is rejected (the
    // cross-file body+contract conflict is enforced by the link resolver).
    for (const auto &d: prog.extDecls) {
      if (globalNames.count(d.name.name)) {
        diags.error("Duplicate global name (decl): " + d.name.name, d.span);
      }
      globalNames.insert(d.name.name);
      checkExtDecl(d, diags);
    }

    // [v0.2.2] Intrinsic declarations. Two intrinsics with the same name
    // but different parameter-type signatures are distinct functions and
    // may coexist (overloading). Same name + same param types = duplicate.
    // Intrinsic names are tracked separately so that same-name intrinsics
    // with different signatures don't collide with each other, but still
    // conflict with non-intrinsic globals (struct, fun, decl).
    std::unordered_set<std::string> intrinsicNames;
    std::unordered_set<std::string> intrinsicSigs;
    for (const auto &d: prog.intrinsics) {
      if (globalNames.count(d.name.name)) {
        diags.error("Duplicate global name (intrinsic): " + d.name.name, d.span);
      }
      std::string sig = d.name.name;
      sig += "(";
      for (size_t i = 0; i < d.params.size(); ++i) {
        if (i > 0)
          sig += ",";
        if (auto bits = TypeUtils::getIntBitWidth(d.params[i].type))
          sig += "i" + std::to_string(*bits);
        else if (auto ft =
                     d.params[i].type ? std::get_if<FloatType>(&d.params[i].type->v) : nullptr)
          // [v0.2.2 D.1+] FP overloads must not collide on the same arity:
          // @to_bits(f32) and @to_bits(f64) are distinct intrinsics with
          // distinct lowerings.  Use the FP precision in the sig string so
          // both can be declared in the same program.
          sig += "f" + std::string(ft->kind == FloatType::Kind::F32 ? "32" : "64");
        else
          sig += "?";
      }
      sig += ")";
      if (intrinsicSigs.count(sig)) {
        diags.error("Duplicate intrinsic signature: " + d.name.name, d.span);
      }
      intrinsicNames.insert(d.name.name);
      intrinsicSigs.insert(sig);
      checkIntrinsicDecl(d, diags);
    }
    for (const auto &name: intrinsicNames)
      globalNames.insert(name);
    return diags.hasErrors() ? refractir::PassResult::Error : refractir::PassResult::Success;
  }

  void SemChecker::checkStruct(const StructDecl &s, DiagBag &diags) {
    std::unordered_set<std::string> fields;
    for (const auto &f: s.fields) {
      if (fields.count(f.name)) {
        diags.error("Duplicate field name: " + f.name, f.span);
      }
      fields.insert(f.name);
    }
  }

  void SemChecker::checkFunction(const FunDecl &f, DiagBag &diags) {
    if (f.blocks.empty()) {
      diags.error("Function must have at least one basic block", f.span);
    }

    checkSigils(f, diags);
    checkDuplicates(f, diags);

    // Check domains
    for (const auto &s: f.syms) {
      if (s.domain) {
        if (auto interval = std::get_if<DomainInterval>(&(*s.domain))) {
          if (interval->lo > interval->hi) {
            diags.error("Invalid symbol domain: lower bound > upper bound", interval->span);
          }
        }
      }
    }
  }

  void SemChecker::checkSigils(const FunDecl &f, DiagBag &diags) {
    // Inside a function, symbols must be local (%?) not global (@?)
    for (const auto &s: f.syms) {
      if (s.name.name.rfind("@?", 0) == 0) {
        diags.error(
            "Global symbol '" + s.name.name +
                "' declared in local scope. Use '%?' for local symbols.",
            s.name.span
        );
      }
    }
  }

  void SemChecker::checkDuplicates(const FunDecl &f, DiagBag &diags) {
    std::unordered_set<std::string> locals;
    std::unordered_set<std::string> labels;

    for (const auto &p: f.params) {
      if (locals.count(p.name.name)) {
        diags.error("Duplicate parameter name: " + p.name.name, p.span);
      }
      locals.insert(p.name.name);
    }

    for (const auto &s: f.syms) {
      if (locals.count(s.name.name)) {
        diags.error("Duplicate name (symbol): " + s.name.name, s.span);
      }
      locals.insert(s.name.name);
    }

    for (const auto &l: f.lets) {
      if (locals.count(l.name.name)) {
        diags.error("Duplicate name (local): " + l.name.name, l.span);
      }
      locals.insert(l.name.name);
    }

    for (const auto &b: f.blocks) {
      if (labels.count(b.label.name)) {
        diags.error("Duplicate block label: " + b.label.name, b.label.span);
      }
      labels.insert(b.label.name);
    }
  }

  // [v0.2.2] §3.4: a contract must have at least one `post` clause. `pre`
  // clauses are optional. Parameter names must be unique.
  void SemChecker::checkExtDecl(const ExtDecl &d, DiagBag &diags) {
    std::unordered_set<std::string> params;
    for (const auto &p: d.params) {
      if (params.count(p.name.name)) {
        diags.error("Duplicate parameter name: " + p.name.name, p.span);
      }
      params.insert(p.name.name);
    }
    if (d.contract) {
      if (d.contract->posts.empty()) {
        diags.error(
            "Contract on '" + d.name.name + "' must contain at least one `post` clause", d.span
        );
      }
    }
  }

  void SemChecker::checkIntrinsicDecl(const IntrinsicDecl &d, DiagBag &diags) {
    std::unordered_set<std::string> params;
    for (const auto &p: d.params) {
      if (params.count(p.name.name)) {
        diags.error("Duplicate parameter name: " + p.name.name, p.span);
      }
      params.insert(p.name.name);
    }

    // [v0.2.2 extra batch A/B] Per-intrinsic signature validation.
    // The interpreter/solver/codegen rely on these invariants; rejecting
    // mis-shaped declarations at check time is cheaper than diagnosing
    // them mid-execution.
    auto kind = getIntrinsicKind(d.name.name);
    if (!kind) {
      diags.error("Unknown intrinsic name: " + d.name.name, d.span);
      return;
    }

    auto retBits = TypeUtils::getIntBitWidth(d.retType);
    auto paramBits = [&](size_t i) -> std::optional<std::uint32_t> {
      if (i >= d.params.size())
        return std::nullopt;
      return TypeUtils::getIntBitWidth(d.params[i].type);
    };
    auto expectArity = [&](size_t expected) {
      if (d.params.size() != expected) {
        diags.error(
            "Intrinsic " + d.name.name + " expects " + std::to_string(expected) +
                " parameter(s), got " + std::to_string(d.params.size()),
            d.span
        );
      }
    };
    auto expectAllInt = [&]() {
      for (size_t i = 0; i < d.params.size(); ++i) {
        if (!paramBits(i))
          diags.error(
              "Intrinsic " + d.name.name + ": parameter " + std::to_string(i) +
                  " must be an integer type",
              d.params[i].span
          );
      }
      if (!retBits)
        diags.error("Intrinsic " + d.name.name + ": return type must be an integer type", d.span);
    };
    auto expectAllSameInt = [&]() {
      expectAllInt();
      if (!retBits)
        return;
      for (size_t i = 0; i < d.params.size(); ++i) {
        auto pb = paramBits(i);
        if (pb && *pb != *retBits) {
          diags.error(
              "Intrinsic " + d.name.name + ": parameter " + std::to_string(i) + " width (i" +
                  std::to_string(*pb) + ") must equal return width (i" + std::to_string(*retBits) +
                  ")",
              d.params[i].span
          );
        }
      }
    };
    auto expectI1Return = [&]() {
      if (!retBits || *retBits != 1) {
        diags.error(
            "Intrinsic " + d.name.name + ": return type must be i1 (predicate intrinsic)", d.span
        );
      }
    };

    // ── v0.2.2 extra D.1 helpers ────────────────────────────────────────
    // Recognise a floating-point type and return its width in bits (32 or 64).
    auto fpBits = [](const TypePtr &t) -> std::optional<std::uint32_t> {
      if (!t)
        return std::nullopt;
      if (auto fp = std::get_if<FloatType>(&t->v)) {
        return fp->kind == FloatType::Kind::F32 ? 32u : 64u;
      }
      return std::nullopt;
    };
    auto paramFp = [&](size_t i) -> std::optional<std::uint32_t> {
      if (i >= d.params.size())
        return std::nullopt;
      return fpBits(d.params[i].type);
    };
    auto retFp = fpBits(d.retType);
    // All parameters and the return are the same FP type. Used by @fabs,
    // @fneg, @copysign.
    auto expectAllSameFp = [&]() {
      if (!retFp) {
        diags.error(
            "Intrinsic " + d.name.name + ": return type must be a floating-point type", d.span
        );
        return;
      }
      for (size_t i = 0; i < d.params.size(); ++i) {
        auto pb = paramFp(i);
        if (!pb) {
          diags.error(
              "Intrinsic " + d.name.name + ": parameter " + std::to_string(i) +
                  " must be a floating-point type",
              d.params[i].span
          );
        } else if (*pb != *retFp) {
          diags.error(
              "Intrinsic " + d.name.name + ": parameter " + std::to_string(i) + " width (f" +
                  std::to_string(*pb) + ") must equal return width (f" + std::to_string(*retFp) +
                  ")",
              d.params[i].span
          );
        }
      }
    };
    // Single FP parameter, i1 return. Used by @signbit.
    auto expectFpToPredicate = [&]() {
      if (!paramFp(0)) {
        diags.error(
            "Intrinsic " + d.name.name + ": parameter 0 must be a floating-point type",
            d.params.empty() ? d.span : d.params[0].span
        );
      }
      expectI1Return();
    };
    // Single FP parameter, integer return whose width equals the FP width
    // (f32→i32, f64→i64). Used by @to_bits.
    auto expectFpToBitsWidthMatch = [&]() {
      auto pb = paramFp(0);
      if (!pb) {
        diags.error(
            "Intrinsic " + d.name.name + ": parameter 0 must be a floating-point type",
            d.params.empty() ? d.span : d.params[0].span
        );
        return;
      }
      if (!retBits) {
        diags.error("Intrinsic " + d.name.name + ": return type must be an integer type", d.span);
        return;
      }
      if (*retBits != *pb) {
        diags.error(
            "Intrinsic " + d.name.name + ": return width (i" + std::to_string(*retBits) +
                ") must equal parameter width (f" + std::to_string(*pb) + ")",
            d.span
        );
      }
    };
    // Single integer parameter, FP return whose width equals the integer
    // width (i32→f32, i64→f64). Used by @from_bits.
    auto expectFromBitsWidthMatch = [&]() {
      auto pb = paramBits(0);
      if (!pb) {
        diags.error(
            "Intrinsic " + d.name.name + ": parameter 0 must be an integer type",
            d.params.empty() ? d.span : d.params[0].span
        );
        return;
      }
      if (!retFp) {
        diags.error(
            "Intrinsic " + d.name.name + ": return type must be a floating-point type", d.span
        );
        return;
      }
      if (*retFp != *pb) {
        diags.error(
            "Intrinsic " + d.name.name + ": return width (f" + std::to_string(*retFp) +
                ") must equal parameter width (i" + std::to_string(*pb) + ")",
            d.span
        );
      }
      if (*pb != 32 && *pb != 64) {
        diags.error(
            "Intrinsic " + d.name.name + ": parameter width i" + std::to_string(*pb) +
                " has no matching FP type — must be i32 or i64",
            d.params[0].span
        );
      }
    };

    switch (*kind) {
      case IntrinsicKind::Abs:
      case IntrinsicKind::Signum:
      case IntrinsicKind::Popcount:
      case IntrinsicKind::Clz:
      case IntrinsicKind::Ctz:
      case IntrinsicKind::Bitreverse:
      case IntrinsicKind::Ilog2:
        expectArity(1);
        expectAllSameInt();
        break;
      case IntrinsicKind::Bswap:
        expectArity(1);
        expectAllSameInt();
        if (retBits && (*retBits % 8) != 0) {
          diags.error(
              "Intrinsic @bswap requires a return type whose width is a multiple of 8, got i" +
                  std::to_string(*retBits),
              d.span
          );
        }
        break;
      case IntrinsicKind::Min:
      case IntrinsicKind::Max:
      case IntrinsicKind::AbsDiff:
      case IntrinsicKind::Midpoint:
      case IntrinsicKind::Rotl:
      case IntrinsicKind::Rotr:
        expectArity(2);
        expectAllSameInt();
        break;
      case IntrinsicKind::Clamp:
        expectArity(3);
        expectAllSameInt();
        break;
      case IntrinsicKind::Parity:
      case IntrinsicKind::IsPow2:
        expectArity(1);
        if (!paramBits(0))
          diags.error(
              "Intrinsic " + d.name.name + ": parameter 0 must be an integer type",
              d.params.empty() ? d.span : d.params[0].span
          );
        expectI1Return();
        break;
      // [v0.2.2 extra batch C] Overflow-aware family — every member is
      // declared at one common iN width across all parameters and the
      // return.
      case IntrinsicKind::WrappingNeg:
      case IntrinsicKind::SaturatingNeg:
        expectArity(1);
        expectAllSameInt();
        break;
      case IntrinsicKind::WrappingAdd:
      case IntrinsicKind::WrappingSub:
      case IntrinsicKind::WrappingMul:
      case IntrinsicKind::WrappingShl:
      case IntrinsicKind::WrappingShr:
      case IntrinsicKind::SaturatingAdd:
      case IntrinsicKind::SaturatingSub:
      case IntrinsicKind::SaturatingMul:
      case IntrinsicKind::DivEuclid:
      case IntrinsicKind::RemEuclid:
        expectArity(2);
        expectAllSameInt();
        break;
      // [v0.2.2 extra batch D.1] FP sign / bit ops (§12.6).
      case IntrinsicKind::Fabs:
      case IntrinsicKind::Fneg:
      // [v0.2.2 extra batch D.4] Correctly-rounded math (§12.6) — every
      // member is unary fN → fN of matching width, same shape as @fabs.
      case IntrinsicKind::Sqrt:
      case IntrinsicKind::Floor:
      case IntrinsicKind::Ceil:
      case IntrinsicKind::Trunc:
        expectArity(1);
        expectAllSameFp();
        break;
      case IntrinsicKind::Copysign:
      // [v0.2.2 extra batch D.3] @fmin / @fmax — same shape as @copysign
      // (two fN parameters of matching width, fN return of the same width).
      case IntrinsicKind::Fmin:
      case IntrinsicKind::Fmax:
        expectArity(2);
        expectAllSameFp();
        break;
      case IntrinsicKind::Signbit:
      // [v0.2.2 extra batch D.2] FP classification predicates (§12.6) —
      // same shape as @signbit (fN parameter, i1 return).
      case IntrinsicKind::IsNormal:
      case IntrinsicKind::IsSubnormal:
        expectArity(1);
        expectFpToPredicate();
        break;
      case IntrinsicKind::ToBits:
        expectArity(1);
        expectFpToBitsWidthMatch();
        break;
      case IntrinsicKind::FromBits:
        expectArity(1);
        expectFromBitsWidthMatch();
        break;
      // Checksum primitives (§12 — see include/analysis/intrinsics.hpp).
      // Their fixed-width signatures don't fit the generic same-width
      // shape, so each gets its own check arm.
      case IntrinsicKind::Crc32Update: {
        // @crc32_update(state: i32, val: iN) : i32
        expectArity(2);
        if (!retBits || *retBits != 32)
          diags.error("Intrinsic @crc32_update: return type must be i32", d.span);
        auto p0 = paramBits(0);
        if (!p0 || *p0 != 32)
          diags.error(
              "Intrinsic @crc32_update: parameter 0 (state) must be i32",
              d.params.empty() ? d.span : d.params[0].span
          );
        if (!paramBits(1))
          diags.error(
              "Intrinsic @crc32_update: parameter 1 (val) must be an integer type",
              d.params.size() < 2 ? d.span : d.params[1].span
          );
        break;
      }
      case IntrinsicKind::CheckChksum: {
        // @check_chksum(expected: i32, actual: i32) : i32 — returns `actual`
        // on equality, asserts on mismatch (with an fprintf diagnostic).
        expectArity(2);
        if (!retBits || *retBits != 32)
          diags.error("Intrinsic @check_chksum: return type must be i32", d.span);
        for (size_t i = 0; i < 2 && i < d.params.size(); ++i) {
          auto pb = paramBits(i);
          if (!pb || *pb != 32)
            diags.error(
                "Intrinsic @check_chksum: parameter " + std::to_string(i) + " must be i32",
                d.params[i].span
            );
        }
        break;
      }
    }
  }

} // namespace refractir
