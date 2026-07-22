#pragma once

// TwinTransform — the equivalence-preserving block-twin rewrite behind rytwin.
//
// For the profiled entry function, TwinTransform walks the executed trace (the
// StateProfile in TransformContext). For each eligible on-path block B, with
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
// needs guard-set ⊇ read-set(B), which planRegion guarantees by rejecting
// any block whose reads are not guardable. Scalar roots cross into the
// guard by value, vector roots per-lane, and aggregate roots by address
// (`ptr [N] T` / `ptr @S` parameters navigated with ptrindex/ptrfield +
// load — all in-bounds by construction, so total on every input).
//
// Candidate blocks may contain memory operations (load/store/addr/ptr
// navigation): a block's effect is the bit-exact frame-state diff, store
// effects surface as diffs of the pointee root, and pointer leaves carry
// provenance so the guard compares them against `addr`-reconstructed
// expected values and the twin reproduces them positionally. Blocks with
// non-intrinsic calls stay ineligible — a callee handed a pointer into an
// outer frame could mutate state the frame diff does not see.
//
// `B'` is solver-generated through the injected `TwinGenFn` (see
// reify/twin_gen.hpp): random statements whose net effect from `s` is
// exactly `s'`, verified bit-exactly. When generation is disabled or no
// attempt verifies, `B'` falls back to a constant reconstruction of the
// leaves the block writes.

#include <memory>

#include "reify/transform.hpp"
#include "reify/twin_gen.hpp"

namespace refractir::reify {

  // How the guard function checks the live-in state against `s`.
  //
  //   Exact     — a conjunction of per-leaf `operand == const`. Readable:
  //               anyone can see it is "state == s".
  //   Bijection — each integer leaf is first run through a nonlinear
  //               bijection on iW, built only from overflow-safe ops
  //               (`x ^= x >>> a`, `x ^= (x>>>a) & (x>>>b)`), and compared
  //               against the pre-mixed constant. A bijection collides with
  //               nothing, so `mix(x) == mix(s) ⟺ x == s`: the guard's
  //               discrimination — and hence the equivalence — is identical
  //               to Exact, but the surface is an opaque `>>> & ^` chain and
  //               the raw expected values never appear, so recovering `s` (to
  //               prove twin ≡ orig) requires inverting a nonlinear map
  //               rather than reading off literals. Float/pointer leaves have
  //               no bijective integer primitive and stay exact. (RefractIR's
  //               `* + << ` are strict-signed — UB on overflow — so the usual
  //               multiply/rotate mixers cannot be used.)
  enum class GuardStyle { Exact, Bijection };

  // How much of the CFG each twin replaces.
  //
  //   Block  — one basic block (the historical unit). The guard fires on the
  //            block's entry state, the twin reproduces its effect, and
  //            control resumes at the block's observed successor.
  //   Region — the maximal single-entry region rooted at the block: every
  //            later block the entry *dominates* on the executed path, up to
  //            the first block it does not (or the function's return). The
  //            guard fires on the region's entry state, the twin reproduces
  //            the region's *net* effect, and control jumps straight to the
  //            region exit — skipping every intermediate block and every loop
  //            iteration in between. Sound by the same argument as Block:
  //            RefractIR is deterministic, so the full entry state fixes the
  //            entire continuation, and the twin is a memoized shortcut for
  //            exactly that state. A whole loop collapses when its header is
  //            the region entry; a straight-line run collapses to a single
  //            block. Block is the degenerate one-block region.
  enum class TwinScope { Block, Region };

  // Build the twin transform. `pTwin` in [0,1] is the per-candidate
  // probability of grafting a twin. `twinGen` generates the twin body; an
  // empty function selects constant reconstruction. `guard` selects the
  // guard-function surface (see GuardStyle); both styles are exact and
  // collision-free. `scope` selects the twin unit (see TwinScope).
  std::unique_ptr<Transform> makeTwinTransform(
      double pTwin, TwinGenFn twinGen = {}, GuardStyle guard = GuardStyle::Exact,
      TwinScope scope = TwinScope::Block
  );

} // namespace refractir::reify
