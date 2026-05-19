#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include "ast/ast.hpp"
#include "reify/cfg_gen.hpp"
#include "reify/expr_gen.hpp"
#include "reify/var_catalogue.hpp"

namespace symir::reify {

  struct FuncGenConfig {
    std::string funcName = "func";
    uint32_t seed = 0;
    int nStmts = 3;
    bool safeOffPath = false;
    bool enableInterestCoefs = true;
    bool enableInterestInits = true;
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
