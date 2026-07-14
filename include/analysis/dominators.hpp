#pragma once

#include <cstddef>
#include <iosfwd>
#include <string>
#include <vector>
#include "analysis/cfg.hpp"

namespace refractir {

  /**
   * Dominator tree over a function's CFG.
   *
   * Built with the Cooper–Harvey–Kennedy iterative algorithm: idoms are
   * intersected over the reverse postorder until a fixpoint. That is
   * near-linear on the small CFGs RefractIR produces and needs no
   * auxiliary forests.
   *
   * Blocks not reachable from the entry are excluded: their slots hold
   * kNone in both `idom` and `rpoNumber`. Callers must treat kNone as
   * "no answer", not as an index.
   */
  struct DomTree {
    static constexpr std::size_t kNone = static_cast<std::size_t>(-1);

    // Immediate dominator per block index. idom[entry] == entry;
    // kNone for unreachable blocks.
    std::vector<std::size_t> idom;
    // Dominator-tree children per block index, ordered by RPO number
    // so traversals are deterministic.
    std::vector<std::vector<std::size_t>> children;
    // Position of each block in the reverse postorder; kNone for
    // unreachable blocks. An idom always has a smaller RPO number than
    // the blocks it dominates, which is what `dominates` relies on.
    std::vector<std::size_t> rpoNumber;

    static DomTree build(const CFG &cfg);

    // Reflexive dominance query: does `a` dominate `b`?
    // False if either block is unreachable.
    bool dominates(std::size_t a, std::size_t b) const;

    // Prints one section per function in a stable label-based format:
    //   domtree @f:
    //     ^entry: (root)
    //     ^b: idom=^entry
    //     ^dead: (unreachable)
    void dump(std::ostream &os, const CFG &cfg, const std::string &funName) const;
  };

} // namespace refractir
