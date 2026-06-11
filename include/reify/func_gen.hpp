#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include "ast/ast.hpp"
#include "reify/cfg_gen.hpp"
#include "reify/checksum.hpp"
#include "reify/expr_gen.hpp"
#include "reify/var_catalogue.hpp"
#include "solver/solver.hpp"

namespace refractir::reify {

  struct FuncGenConfig {
    std::string funcName = "func";
    uint32_t seed = 0;
    int nStmts = 3;
    // Off-path blocks are never executed at the solved inputs, so their
    // volume costs the solver nothing while widening the compiler-facing
    // optimization surface. nStmts and exprCfg.{min,max}Atoms describe
    // ON-path blocks; off-path blocks scale all three by this factor
    // (--off-path-multiplier, rounded to the nearest int, default 2x).
    double offPathMultiplier = 2.0;
    bool enableInterestCoefs = true;
    bool enableInterestInits = true;
    bool enableIntrinsics = true;
    // Probability that a new on-path coef sym gets a `|c| > 2^20`
    // require, replacing the old unconditional `c != 0 ∧ c != 1 ∧ c != -1`
    // triple. With the triple in place the solver clusters every coef at
    // ±2 (the smallest values surviving the filter); R5 trades that
    // floor for a real diversity guarantee.
    double pLargeCoef = 0.3;
    // Magnitude threshold T for the `|c| > T` interest require, set by
    // --large-coef. Clamped per-coef to the coef's domain ∩ type range, so
    // a value wider than --coef-domain degrades to the largest in-domain
    // magnitude rather than going UNSAT.
    int64_t largeCoefThreshold = 1 << 20;
    ExprGenConfig exprCfg;
    // Sym counter domains
    int64_t coefLo = -8, coefHi = 8;
    int64_t valueLo = -128, valueHi = 127;
    int64_t indexLo = 1, indexHi = 30;
  };

  struct FuncGenResult {
    refractir::Program prog;
    std::vector<std::string> pathLabels; // ["^entry", "^b0", ...]
  };

  FuncGenResult genFunction(
      const RyCFG &cfg, const std::vector<std::string> &path, const VarCatalogue &vars,
      const FuncGenConfig &fcfg
  );

} // namespace refractir::reify
