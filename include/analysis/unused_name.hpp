#pragma once

#include <string>
#include "analysis/pass_manager.hpp"

namespace refractir {

  class UnusedNameAnalysis : public refractir::FunctionPass {
  public:
    std::string name() const override { return "UnusedNameAnalysis"; }

    refractir::PassResult run(FunDecl &f, DiagBag &diags) override;
  };

} // namespace refractir
