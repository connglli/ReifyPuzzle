#pragma once

// TwinPass — the equivalence-preserving block-twin rewrite behind rytwin.
//
// For the profiled entry function, TwinPass walks the executed trace (the
// StateProfile in PassCtx). For each eligible on-path block B, with
// probability `pTwin`, it grafts an equivalent alternative:
//
//     ^X (guard):  br call @__twg_<fn>_<X>(<state>) != 0, ^X__twin, ^X__orig
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
// The guard is a per-site generated function `@__twg_<fn>_<label> : i1`
// that consumes the ENTIRE definitely-initialized state at B's entry — a
// conjunction of per-leaf equalities, which is total (no UB) and
// collision-free, so it can only fire on exactly `s`. Covering the whole
// state (not just B's read set) maximizes discrimination; soundness only
// needs guard-set ⊇ read-set(B), which planBlock guarantees by rejecting
// any block whose reads are not guardable. Scalar roots cross into the
// guard by value, vector roots per-lane, and aggregate roots by address
// (`ptr [N] T` / `ptr @S` parameters navigated with ptrindex/ptrfield +
// load — all in-bounds by construction, so total on every input).
//
// v1 scope: candidate blocks are memory-free and call-free (no
// load/store/addr/ptr navigation, intrinsic calls only) and every leaf
// they read or write is concrete (no pointer / undef). `B'` is currently
// a constant reconstruction of `s'` and will grow richer
// (solver-generated) later.

#include <memory>

#include "reify/pass.hpp"

namespace refractir::reify {

  // Build the twin pass. `pTwin` in [0,1] is the per-candidate-block
  // probability of grafting a twin.
  std::unique_ptr<Pass> makeTwinPass(double pTwin);

} // namespace refractir::reify
