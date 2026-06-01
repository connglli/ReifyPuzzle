#pragma once

#include <cstdint>
#include <random>
#include <set>
#include <string>
#include <vector>
#include "analysis/intrinsics.hpp"
#include "ast/ast.hpp"
#include "reify/type_gen.hpp"
#include "reify/var_catalogue.hpp"

namespace symir::reify {

  // Key for tracking which (intrinsic, bitwidth) pairs have been used.
  // Now that the toolchain supports same-name intrinsics with different
  // signatures, each (kind, width) pair gets its own IntrinsicDecl.
  using IntrinsicUseKey = std::pair<IntrinsicKind, uint32_t>;

  // ---------------------------------------------------------------------------
  // SymCounter — tracks generated symbols, produces declarations
  // ---------------------------------------------------------------------------

  struct SymEntry {
    std::string name; // "%?sN"
    SymKind kind;
    TypePtr type;   // actual type of this sym (not always i32)
    int64_t lo, hi; // domain bounds
  };

  struct SymCounter {
    std::vector<SymEntry> entries;
    int n = 0;

    // Domains
    int64_t coefLo = -8, coefHi = 8;
    int64_t valueLo = -128, valueHi = 127;
    int64_t indexLo = 1, indexHi = 30;

    // Generate next sym of given kind and type
    std::string next(SymKind kind, TypePtr type);

    // Convenience: next coef sym of given type (most common)
    std::string nextCoef(const TypePtr &type);

    // Next value sym (i32)
    std::string nextValue();

    // Next index sym (i32)
    std::string nextIndex();

    int countOfKind(SymKind kind) const;
    std::vector<std::string> namesOfKindSince(SymKind kind, int since) const;
    std::vector<SymDecl> makeDecls() const;
  };

  // ---------------------------------------------------------------------------
  // ExprGenConfig
  // ---------------------------------------------------------------------------

  struct ExprGenConfig {
    bool enableAllOps = true; // bitwise, shifts
    bool enableDiv = true;    // div/mod with concrete denominators
    bool enableSelect = true; // select ternary
    bool enableFp = true;
    bool enableIntrinsics = true;
    // Mutable set populated during expression generation. genFunction
    // reads it afterward to emit IntrinsicDecl entries. May be nullptr
    // (e.g. in tests) — intrinsic generation is silently skipped.
    std::set<IntrinsicUseKey> *usedIntrinsics = nullptr;
  };

  // ---------------------------------------------------------------------------
  // Core expression builder
  // ---------------------------------------------------------------------------

  // Generate an Expr of type targetType.
  // onPath=true: use symbols (sym counter). onPath=false: use concrete literals.
  // sym may be nullptr for off-path generation.
  Expr genExpr(
      std::mt19937 &rng,
      SymCounter *sym, // nullptr-safe: if nullptr, always concrete
      const VarCatalogue &vars, const TypePtr &targetType, bool onPath, const ExprGenConfig &cfg
  );

  // Generate a branch Cond using random scalar var from available vars.
  Cond genCond(
      std::mt19937 &rng, SymCounter *sym, const VarCatalogue &vars, bool onPath,
      const ExprGenConfig &cfg
  );

  // Generate interest requires for new coef syms since coefCountBefore.
  // These are RequireInstrs that exclude trivial values (0, 1, -1).
  std::vector<Instr> interestCoefRequires(const SymCounter &sym, int coefCountBefore);

  // Generate N statements (assign + store mix) for a block.
  // onPath=true: uses sym. onPath=false: concrete only. safeOffPath: add UB guards.
  std::vector<Instr> genBlockStmts(
      std::mt19937 &rng,
      SymCounter *sym, // null for off-path
      const VarCatalogue &vars, int nStmts, bool onPath, bool safeOffPath, const ExprGenConfig &cfg
  );

} // namespace symir::reify
