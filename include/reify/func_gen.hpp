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

namespace symir::reify {

  struct FuncGenConfig {
    std::string funcName = "func";
    uint32_t seed = 0;
    int nStmts = 3;
    bool safeOffPath = false;
    bool enableInterestCoefs = true;
    bool enableInterestInits = true;
    bool enableIntrinsics = true;
    // Probability that a new on-path coef sym gets a `|c| > 2^20`
    // require, replacing the old unconditional `c != 0 ∧ c != 1 ∧ c != -1`
    // triple. With the triple in place the solver clusters every coef at
    // ±2 (the smallest values surviving the filter); R5 trades that
    // floor for a real diversity guarantee.
    double pLargeCoef = 0.3;
    ExprGenConfig exprCfg;
    // Sym counter domains
    int64_t coefLo = -8, coefHi = 8;
    int64_t valueLo = -128, valueHi = 127;
    int64_t indexLo = 1, indexHi = 30;
  };

  struct FuncGenResult {
    symir::Program prog;
    std::vector<std::string> pathLabels; // ["^entry", "^b0", ...]
  };

  FuncGenResult genFunction(
      const RyCFG &cfg, const std::vector<std::string> &path, const VarCatalogue &vars,
      const FuncGenConfig &fcfg
  );

} // namespace symir::reify
