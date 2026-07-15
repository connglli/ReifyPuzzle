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
    // condition is emitted. Both arms are present in a freshly built
    // tree; StructuredLowering may drop an emptied else arm (nullptr)
    // and set `negate` when the arms had to swap.
    struct If {
      std::size_t block;
      NodePtr thenBr, elseBr;
      bool negate = false;
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

    // --- Nodes produced only by StructuredLowering (never by
    // Structurizer::build). They express multi-level exits with
    // one-shot guard flags for targets without labeled break
    // (python now; structured C later). Targets with native
    // multi-level branches (WASM `br N`) consume the unlowered
    // tree instead. Flag indices refer to `flagNames`.

    // A while-condition loop (peepholed `while True` whose header had
    // no instructions): `while cond:` / `while not cond:`, where the
    // condition is the header block's BrTerm cond.
    struct CondLoop {
      int loopId;
      std::size_t header;
      bool negate;
      NodePtr body;
    };

    // A do-while loop (peepholed `while True` whose body *ends* with a
    // single-level `if cond: break` and contains no other continue
    // site binding to this loop — C's `continue` inside do-while
    // evaluates the condition instead of re-entering the body). The
    // body always runs once, then repeats while the condition holds.
    // `latch` is the block whose BrTerm supplies the condition;
    // `negate` applies to the *repeat* condition (`while (!(c))`).
    // Targets without do-while (python) re-expand it to the exact
    // pre-peephole form.
    struct DoWhile {
      int loopId;
      std::size_t latch;
      bool negate;
      NodePtr body;
    };

    // `<flag> = True`.
    struct SetFlag {
      int flag;
    };

    // Cascade after an inner loop: `if <flag>: break`, resetting the
    // flag first when this is the last cascade of its escape.
    struct FlagBreak {
      int flag;
      bool isFinal;
    };

    // Final cascade of a multi-level continue:
    // `if <flag>: <flag> = False; continue`.
    struct FlagContinue {
      int flag;
    };

    // A skipped join subtree: `if not <flag1> (and not <flag2>)…:`.
    struct Guarded {
      std::vector<int> flags;
      NodePtr body;
    };

    // `<flag> = False` (emitted where a jump-join target is reached).
    struct ResetFlag {
      int flag;
    };

    struct Node {
      std::variant<
          Seq, BlockStmts, If, Loop, Break, Continue, FallThrough, JumpJoin, Return, Trap, CondLoop,
          DoWhile, SetFlag, FlagBreak, FlagContinue, Guarded, ResetFlag>
          v;
    };

    NodePtr root;
    // Guard-flag names allocated by StructuredLowering ("_brk_done",
    // "_cnt_oh", "_go_f2", …). Emitters declare/initialize them at
    // function entry.
    std::vector<std::string> flagNames;
    // Set by StructuredLowering; selects the dump section header.
    bool lowered = false;

    // Prints an indented label-based rendering, one section per
    // function (see test/analysis/control_tree_*.sir.expected). A
    // lowered tree dumps with the "lowered-tree" section header
    // instead (test/analysis/lowered_*.sir.expected).
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
