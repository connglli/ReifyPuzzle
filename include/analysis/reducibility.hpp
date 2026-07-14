#pragma once

#include <cstddef>
#include <string>
#include <vector>
#include "analysis/cfg.hpp"
#include "analysis/dominators.hpp"
#include "analysis/pass_manager.hpp"

namespace refractir {

  /**
   * Reducibility verdict for a function's CFG.
   *
   * A CFG is reducible iff every retreating edge (an edge u -> v with
   * rpoNumber[v] <= rpoNumber[u]) is a true back edge, i.e. its target
   * dominates its source. Retreating edges that fail the dominance test
   * are reported individually so diagnostics can point at the exact
   * branch that enters a loop past its header — something the classic
   * T1/T2 interval-collapse test cannot name.
   */
  struct ReducibilityResult {
    struct Offender {
      std::size_t src, dst;
    };

    // Retreating edges whose target does not dominate their source,
    // in block-index order. Empty iff the CFG is reducible.
    std::vector<Offender> offenders;

    bool reducible() const { return offenders.empty(); }

    static ReducibilityResult check(const CFG &cfg, const DomTree &dt);
  };

  /**
   * Diagnostic surface for ReducibilityResult: rejects irreducible
   * functions with an error per offending edge. Registered by drivers
   * whose target cannot express irreducible control flow (the python
   * backend), or unconditionally via `symirc --require-reducible`.
   */
  class ReducibilityCheck : public FunctionPass {
  public:
    PassResult run(FunDecl &fun, DiagBag &diags) override;

    std::string name() const override { return "ReducibilityCheck"; }
  };

} // namespace refractir
