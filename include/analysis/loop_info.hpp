#pragma once

#include <cstddef>
#include <iosfwd>
#include <string>
#include <utility>
#include <vector>
#include "analysis/cfg.hpp"
#include "analysis/dominators.hpp"

namespace refractir {

  /**
   * One natural loop. All back edges targeting the same header are
   * merged into a single Loop with multiple latches — that is the only
   * "normalization" structured emission needs: the header is unique by
   * construction on a reducible CFG, extra latches are just extra
   * `continue` sites, and each exit edge independently lowers to a
   * `break`. No preheaders or dedicated exit blocks are synthesized;
   * the block list is never mutated.
   */
  struct Loop {
    std::size_t header = 0;
    // Back-edge sources, RPO-sorted.
    std::vector<std::size_t> latches;
    // Natural loop body including the header, RPO-sorted (so the
    // header is always first).
    std::vector<std::size_t> blocks;
    // Edges (src in loop, dst outside), ordered by src RPO then
    // successor order.
    std::vector<std::pair<std::size_t, std::size_t>> exitEdges;
    // Index into LoopInfo::loops; -1 = top-level loop.
    int parent = -1;
    std::vector<int> children;
    int depth = 1;
  };

  /**
   * The loop nesting forest of a function.
   *
   * Only true back edges (retreating edges whose target dominates
   * their source) form loops, so on an irreducible CFG the result
   * silently ignores the irreducible cycles — run ReducibilityCheck
   * first when that matters.
   */
  struct LoopInfo {
    // Outer loops before inner (sorted by header RPO number).
    std::vector<Loop> loops;
    // Innermost loop index per block; -1 if the block is in no loop.
    std::vector<int> innermostLoop;

    static LoopInfo build(const CFG &cfg, const DomTree &dt);

    // Prints one section per function in a stable label-based format:
    //   loops @f:
    //     loop 0: header=^h depth=1 parent=none
    //       latches: ^latch
    //       blocks: ^h ^b ^latch
    //       exits: ^h->^exit
    // or "  (none)" for a loop-free function.
    void dump(std::ostream &os, const CFG &cfg, const std::string &funName) const;
  };

} // namespace refractir
