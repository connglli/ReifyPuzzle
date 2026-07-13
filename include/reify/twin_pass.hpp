#pragma once

// TwinPass — the equivalence-preserving block-twin rewrite behind rytwin.
//
// For the profiled entry function, TwinPass walks the executed trace (the
// StateProfile in PassCtx). For each eligible on-path block B, with
// probability `pTwin`, it grafts an equivalent alternative:
//
//     ^X (guard):  if state()==s  ->  ^X__twin  else  ^X__orig
//     ^X__twin:    B'  ->  ^X__merge      (B' reproduces B's effect at s)
//     ^X__orig:    B   ->  ^X__merge      (the original block body)
//     ^X__merge:   <B's original terminator>
//
// `s` is the concrete state B sees on the profiled input (from the
// profile); `B'` reproduces `s' = B(s)` for the variables B writes; and
// the guard fires only when the live-in state equals `s`, so on the
// profiled input the twin runs and on every other state the original runs.
// Thus p1(i) == p2(i) for every input i.
//
// v1 scope: candidate blocks are scalar-only and side-effect-free (no
// load/store/addr/ptr navigation, no calls, only whole-scalar-local
// assignments). The guard is `Exact` — a conjunction of per-variable
// equalities over the live-in scalars, which is total (no UB) and
// collision-free, so the equivalence holds on every input, not just the
// profiled one. `Crc32` (a compact checksum guard) is a planned refinement;
// `B'` is currently a constant reconstruction of `s'` and will grow richer
// (solver-synthesized) later.

#include <memory>

#include "reify/pass.hpp"

namespace refractir::reify {

  enum class TwinGuard {
    Exact, // per-variable equality conjunction (total, collision-free)
    Crc32, // checksum of the live-in state (planned)
  };

  // Build the twin pass. `pTwin` in [0,1] is the per-candidate-block
  // probability of grafting a twin; `guard` selects the state() check.
  std::unique_ptr<Pass> makeTwinPass(double pTwin, TwinGuard guard);

} // namespace refractir::reify
