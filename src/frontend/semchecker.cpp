#include "frontend/semchecker.hpp"
#include <algorithm>
#include "analysis/intrinsics.hpp"
#include "analysis/type_utils.hpp"

namespace symir {

  symir::PassResult SemChecker::run(Program &prog, DiagBag &diags) {
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
    return diags.hasErrors() ? symir::PassResult::Error : symir::PassResult::Success;
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
    }
  }

} // namespace symir
