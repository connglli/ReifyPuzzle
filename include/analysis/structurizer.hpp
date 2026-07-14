#pragma once

#include <cstddef>
#include <iosfwd>
#include <memory>
#include <string>
#include <variant>
#include <vector>
#include "analysis/cfg.hpp"
#include "analysis/dominators.hpp"
#include "analysis/loop_info.hpp"
#include "ast/ast.hpp"

namespace refractir {

  /**
   * A structured control tree reconstructed from a reducible CFG.
   *
   * Built with the dominator-tree translation of Ramsey ("Beyond
   * Relooper", ICFP 2022): a block's dominator-tree children that are
   * merge nodes become a follower sequence after the block's own
   * content; single-forward-predecessor children inline into the
   * branch arm that reaches them; loop headers wrap their natural loop
   * body in a Loop node whose out-of-loop dominated blocks follow it.
   * Total for every reducible CFG — no node splitting, no CFG
   * mutation.
   *
   * The tree is target-neutral: transfers say *what* must happen
   * (`Break{levels=2}`, `JumpJoin`), not how a backend expresses it. A
   * target with labeled break lowers them natively; the python emitter
   * lowers levels>1 and JumpJoin to guard flags.
   */
  struct ControlTree {
    struct Node;
    using NodePtr = std::unique_ptr<Node>;

    // Ordered children emitted at the same nesting level.
    struct Seq {
      std::vector<NodePtr> items;
    };

    // The straight-line instructions of one block (terminator excluded;
    // the parent lowers it into the nodes that follow).
    struct BlockStmts {
      std::size_t block;
    };

    // A conditional terminator. `block` carries the BrTerm whose
    // condition is emitted; both arms are always present.
    struct If {
      std::size_t block;
      NodePtr thenBr, elseBr;
    };

    // A natural loop ("while True" until a Break leaves it).
    struct Loop {
      int loopId;
      std::size_t header;
      NodePtr body;
    };

    // Leave `levels` enclosing loops and resume at pending join
    // `target` (which follows the outermost loop left, with no other
    // pending joins in between).
    struct Break {
      std::size_t target;
      int levels; // >= 1
    };

    // Back edge: re-enter the loop headed at `header` after first
    // leaving `levels` inner loops (0 = innermost loop's own latch).
    struct Continue {
      std::size_t header;
      int levels;
    };

    // Fall through to the next pending join at this nesting level.
    struct FallThrough {
      std::size_t target;
    };

    // Forward jump to pending join `target` that must skip at least
    // one intermediate join subtree (after leaving `levels` loops):
    // no native construct expresses it, so emitters guard the skipped
    // subtrees with a flag.
    struct JumpJoin {
      std::size_t target;
      int levels; // >= 0
    };

    // A `ret` terminator; `block` carries the RetTerm.
    struct Return {
      std::size_t block;
    };

    // An `unreachable` terminator.
    struct Trap {
      std::size_t block;
    };

    struct Node {
      std::variant<Seq, BlockStmts, If, Loop, Break, Continue, FallThrough, JumpJoin, Return, Trap>
          v;
    };

    NodePtr root;

    // Prints an indented label-based rendering, one section per
    // function (see test/analysis/control_tree_*.sir.expected).
    void dump(std::ostream &os, const CFG &cfg, const std::string &funName) const;
  };

  struct Structurizer {
    /**
     * Requires a reducible CFG (run ReducibilityCheck first); the
     * LoopInfo must come from the same CFG/DomTree pair.
     */
    static ControlTree
    build(const FunDecl &f, const CFG &cfg, const DomTree &dt, const LoopInfo &li);
  };

} // namespace refractir
