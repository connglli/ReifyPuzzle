#pragma once

// twin_gen — solver-generated twin blocks for rytwin.
//
// Given a block's entry state `s` and exit state `s'` (as concrete value
// trees per root), generateTwin synthesizes an instruction sequence whose
// net effect from `s` is exactly `s'` — the rysmith way: random statements
// with `%?` symbols (UB-safety requires spliced automatically), a fresh
// additive correction symbol per touched leaf so the target is always
// reachable, and one equality `require` per leaf pinning the final state
// to `s'`. The mini-program is solved in-process with the SMT solver,
// concretized by printing with the model and re-parsing, then cross-checked
// bit-exactly by running the interpreter (IEEE `==` conflates ±0.0; the
// bit-exact re-run does not). The equality requires are scaffolding and are
// stripped from the returned instructions.
//
// The TwinTransform consumes this through the `TwinGenFn` callback, injected by
// the rytwin driver: the pass library stays free of a solver link
// dependency (rylink links reify without the solver backend).

#include <functional>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "ast/ast.hpp"
#include "reify/state_profile.hpp"
#include "solver/solver.hpp"

namespace refractir::reify {

  // A pointer leaf of a root, with the lvalues whose addresses reproduce
  // the captured pointer at block entry and exit (nullopt = null pointer).
  // The mini-program declares the cell `null`, assigns `addr <initTarget>`
  // before the generated body, and reassigns `addr <finalTarget>` after it
  // — sound whatever the body did to the cell in between.
  struct TwinGenPtrFix {
    std::vector<Access> path; // leaf path within the root
    TypePtr type;             // static `ptr T` of the cell
    std::optional<LValue> initTarget;
    std::optional<LValue> finalTarget;
  };

  // One state root the twin must model: its declaration shape in the entry
  // function plus its concrete entry / exit values.
  struct TwinGenRoot {
    std::string name;
    TypePtr type;
    bool isParam = false;                // immutable in the mini-program (never written)
    StateValue init;                     // value at block entry (s)
    StateValue target;                   // required value at block exit (s')
    std::vector<TwinGenPtrFix> ptrFixes; // every pointer leaf of the root
  };

  struct TwinGenConfig {
    int nStmts = 3;             // random statements per attempt
    int retries = 3;            // generation attempts before giving up
    uint32_t timeoutMs = 10000; // per-attempt SMT timeout
  };

  // A verified twin body plus the intrinsic declarations its instructions
  // call — the grafting pass merges those into the host program.
  struct TwinGenResult {
    std::vector<Instr> instrs;
    std::vector<IntrinsicDecl> intrinsics;
  };

  // Generator callback used by TwinTransform. Returns the concrete twin-block
  // body, or nullopt when no attempt verified (the pass then falls back to
  // constant reconstruction).
  using TwinGenFn = std::function<std::optional<TwinGenResult>(
      const Program &, const std::vector<TwinGenRoot> &, std::mt19937 &
  )>;

  std::optional<TwinGenResult> generateTwin(
      const Program &prog, const std::vector<TwinGenRoot> &roots, std::mt19937 &rng,
      const SymbolicExecutor::SolverFactory &solverFactory, const TwinGenConfig &cfg
  );

} // namespace refractir::reify
