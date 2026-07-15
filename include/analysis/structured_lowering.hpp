#pragma once

#include "analysis/structurizer.hpp"

namespace refractir {

  /**
   * Lowers a ControlTree for targets without labeled break / native
   * multi-level branches (python now; structured C later):
   *
   *   - Break{levels=L>1}   → SetFlag + break, then a FlagBreak
   *                           cascade after each of the L-1 enclosing
   *                           loops (the last cascade resets the flag).
   *   - Continue{levels=k>0}→ SetFlag + break, k-1 FlagBreak cascades,
   *                           then a FlagContinue inside the target
   *                           loop.
   *   - JumpJoin            → SetFlag (+ break/cascades when loops are
   *                           crossed); the skipped join subtrees are
   *                           wrapped in Guarded and the flag resets at
   *                           the target subtree.
   *   - FallThrough nodes and tail-position `continue levels=0` are
   *     dropped; emptied If arms are removed (negating the condition
   *     when only the then arm emptied).
   *   - `while True` loops whose header has no instructions and whose
   *     header If has a single-level-break arm become CondLoop
   *     ("while cond:").
   *   - `while True` loops whose body *ends* with a single-level
   *     `if cond: break` and contains no other continue site binding
   *     to the loop become DoWhile ("do { body } while (cond);").
   *     Targets without do-while (python) re-expand the node to the
   *     exact pre-peephole form, so their output is unchanged.
   *
   * The result contains no FallThrough / JumpJoin nodes, every Break
   * has levels == 1, and every Continue has levels == 0 — i.e. only
   * constructs a single-level-break language expresses directly.
   * Flags are one-shot: False except between their SetFlag and the
   * final cascade / reset, so re-entering the region is safe.
   */
  struct StructuredLowering {
    static ControlTree run(ControlTree tree, const FunDecl &f, const CFG &cfg);
  };

} // namespace refractir
