#pragma once

#include <string>
#include "analysis/pass_manager.hpp"

namespace refractir {

  /**
   * Performs reachability analysis on a function's CFG.
   * Identifies unreachable basic blocks and reports them as warnings.
   */
  class ReachabilityAnalysis : public refractir::FunctionPass {
  public:
    std::string name() const override { return "ReachabilityAnalysis"; }

    /**
     * Executes the analysis on the function.
     */
    refractir::PassResult run(FunDecl &f, DiagBag &diags) override;
  };

} // namespace refractir
