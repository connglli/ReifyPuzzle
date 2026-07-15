#include "analysis/structured_lowering.hpp"
#include <cassert>
#include <unordered_map>

namespace refractir {

  namespace {

    using Node = ControlTree::Node;
    using NodePtr = ControlTree::NodePtr;

    template<typename T>
    NodePtr make(T &&alt) {
      return std::make_unique<Node>(Node{std::forward<T>(alt)});
    }

    // An in-flight multi-level transfer travelling up the tree from
    // its SetFlag site. `remaining` counts loop exits still owed
    // (cascades to place at successive Loop unwinds); a Join escape
    // whose remaining hits 0 switches to the guard phase and resolves
    // in the first enclosing Seq that contains its target subtree.
    struct Escape {
      int flag;
      int remaining;
      enum class Kind { Break, Continue, Join } kind;
      std::size_t target;
    };

    class Lowerer {
    public:
      Lowerer(ControlTree &tree, const FunDecl &f, const CFG &cfg) :
          tree_(tree), f_(f), cfg_(cfg) {}

      void run() {
        auto esc = lowerNode(tree_.root, false);
        assert(esc.empty() && "escape leaked past the function root");
        (void) esc;
      }

    private:
      static std::string stripSigil(const std::string &name) {
        std::size_t start = 0;
        if (!name.empty() && (name[0] == '@' || name[0] == '%' || name[0] == '^'))
          start = 1;
        return name.substr(start);
      }

      int getFlag(const std::string &prefix, std::size_t block) {
        std::string name = prefix + stripSigil(cfg_.blocks[block]);
        auto it = flagIds_.find(name);
        if (it != flagIds_.end())
          return it->second;
        int id = static_cast<int>(tree_.flagNames.size());
        tree_.flagNames.push_back(name);
        flagIds_.emplace(name, id);
        return id;
      }

      static constexpr std::size_t kNoBlock = static_cast<std::size_t>(-1);

      // The block a subtree "is": used to recognize a pending join's
      // target subtree in its enclosing Seq.
      static std::size_t firstBlock(const Node &n) {
        return std::visit(
            [](const auto &alt) -> std::size_t {
              using T = std::decay_t<decltype(alt)>;
              if constexpr (std::is_same_v<T, ControlTree::Seq>) {
                for (const auto &item: alt.items)
                  if (item) {
                    std::size_t b = firstBlock(*item);
                    if (b != kNoBlock)
                      return b;
                  }
                return kNoBlock;
              } else if constexpr (std::is_same_v<T, ControlTree::BlockStmts>) {
                return alt.block;
              } else if constexpr (std::is_same_v<T, ControlTree::If>) {
                return alt.block;
              } else if constexpr (std::is_same_v<T, ControlTree::Loop>) {
                return alt.header;
              } else if constexpr (std::is_same_v<T, ControlTree::CondLoop>) {
                return alt.header;
              } else if constexpr (std::is_same_v<T, ControlTree::DoWhile>) {
                // The loop's header is the first block of the body.
                return alt.body ? firstBlock(*alt.body) : kNoBlock;
              } else {
                return kNoBlock;
              }
            },
            n.v
        );
      }

      static bool isEmpty(const NodePtr &n) {
        if (!n)
          return true;
        if (auto seq = std::get_if<ControlTree::Seq>(&n->v)) {
          for (const auto &item: seq->items)
            if (!isEmpty(item))
              return false;
          return true;
        }
        return false;
      }

      static bool isSingleLevelBreak(const NodePtr &n) {
        if (!n)
          return false;
        auto br = std::get_if<ControlTree::Break>(&n->v);
        return br && br->levels == 1;
      }

      // Does this subtree contain a continue site binding to the
      // innermost enclosing loop (a bare `continue` or FlagContinue
      // not nested in an inner loop)? Such a site vetoes the do-while
      // peephole: C's `continue` inside do-while evaluates the
      // condition instead of unconditionally re-entering the body.
      static bool hasLoopContinueSite(const NodePtr &n) {
        if (!n)
          return false;
        return std::visit(
            [](const auto &alt) -> bool {
              using T = std::decay_t<decltype(alt)>;
              if constexpr (std::is_same_v<T, ControlTree::Continue> ||
                            std::is_same_v<T, ControlTree::FlagContinue>) {
                return true;
              } else if constexpr (std::is_same_v<T, ControlTree::Seq>) {
                for (const auto &item: alt.items)
                  if (hasLoopContinueSite(item))
                    return true;
                return false;
              } else if constexpr (std::is_same_v<T, ControlTree::If>) {
                return hasLoopContinueSite(alt.thenBr) || hasLoopContinueSite(alt.elseBr);
              } else if constexpr (std::is_same_v<T, ControlTree::Guarded>) {
                return hasLoopContinueSite(alt.body);
              } else {
                // Loop / CondLoop / DoWhile bodies rebind continue;
                // the remaining leaves contain none.
                return false;
              }
            },
            n->v
        );
      }

      // Lowers `n` in place. May null `n` out entirely (dropped
      // FallThrough / tail continue). `tail` means falling off the end
      // of `n` re-enters the innermost enclosing loop.
      std::vector<Escape> lowerNode(NodePtr &n, bool tail) {
        if (!n)
          return {};
        // Leaf drops first.
        if (std::holds_alternative<ControlTree::FallThrough>(n->v)) {
          n = nullptr;
          return {};
        }
        if (auto cont = std::get_if<ControlTree::Continue>(&n->v)) {
          if (cont->levels == 0) {
            if (tail)
              n = nullptr; // implicit: end of loop body continues
            return {};
          }
          // Multi-level continue: flag + break, FlagContinue placed at
          // the target loop's unwind.
          int flag = getFlag("_cnt_", cont->header);
          Escape e{flag, cont->levels, Escape::Kind::Continue, cont->header};
          ControlTree::Seq seq;
          seq.items.push_back(make(ControlTree::SetFlag{flag}));
          seq.items.push_back(make(ControlTree::Break{cont->header, 1}));
          n = make(std::move(seq));
          return {e};
        }
        if (auto br = std::get_if<ControlTree::Break>(&n->v)) {
          if (br->levels <= 1)
            return {};
          int flag = getFlag("_brk_", br->target);
          Escape e{flag, br->levels - 1, Escape::Kind::Break, br->target};
          ControlTree::Seq seq;
          seq.items.push_back(make(ControlTree::SetFlag{flag}));
          seq.items.push_back(make(ControlTree::Break{br->target, 1}));
          n = make(std::move(seq));
          return {e};
        }
        if (auto jj = std::get_if<ControlTree::JumpJoin>(&n->v)) {
          int flag = getFlag("_go_", jj->target);
          Escape e{flag, jj->levels > 0 ? jj->levels - 1 : 0, Escape::Kind::Join, jj->target};
          if (jj->levels == 0) {
            n = make(ControlTree::SetFlag{flag});
          } else {
            ControlTree::Seq seq;
            seq.items.push_back(make(ControlTree::SetFlag{flag}));
            seq.items.push_back(make(ControlTree::Break{jj->target, 1}));
            n = make(std::move(seq));
          }
          return {e};
        }
        if (std::holds_alternative<ControlTree::Seq>(n->v))
          return lowerSeq(n, tail);
        if (std::holds_alternative<ControlTree::If>(n->v))
          return lowerIf(n, tail);
        if (std::holds_alternative<ControlTree::Loop>(n->v))
          return lowerLoop(n);
        return {}; // BlockStmts / Return / Trap
      }

      std::vector<Escape> lowerSeq(NodePtr &n, bool tail) {
        auto &seq = std::get<ControlTree::Seq>(n->v);
        std::vector<Escape> pending;

        struct Guard {
          int flag;
          std::size_t target;
        };

        std::vector<Guard> guards;
        std::vector<NodePtr> out;

        const std::size_t count = seq.items.size();
        for (std::size_t i = 0; i < count; ++i) {
          NodePtr item = std::move(seq.items[i]);
          auto esc = lowerNode(item, tail && i + 1 == count);
          if (item) {
            // Resolve active jump-join guards against this item: the
            // target subtree resets its flag and stays unguarded;
            // anything else pending gets wrapped.
            std::size_t fb = firstBlock(*item);
            std::vector<Guard> still;
            for (const auto &g: guards) {
              if (g.target == fb) {
                out.push_back(make(ControlTree::ResetFlag{g.flag}));
              } else {
                still.push_back(g);
              }
            }
            guards = std::move(still);
            if (!guards.empty()) {
              ControlTree::Guarded gd;
              for (const auto &g: guards)
                gd.flags.push_back(g.flag);
              gd.body = std::move(item);
              item = make(std::move(gd));
            }
            out.push_back(std::move(item));
          }
          for (auto &e: esc) {
            if (e.kind == Escape::Kind::Join && e.remaining == 0)
              guards.push_back({e.flag, e.target});
            else
              pending.push_back(e);
          }
        }
        // Guards whose target lives in an enclosing Seq keep guarding
        // there.
        for (const auto &g: guards)
          pending.push_back({g.flag, 0, Escape::Kind::Join, g.target});

        seq.items = std::move(out);
        return pending;
      }

      std::vector<Escape> lowerIf(NodePtr &n, bool tail) {
        auto &iff = std::get<ControlTree::If>(n->v);
        auto esc = lowerNode(iff.thenBr, tail);
        auto escElse = lowerNode(iff.elseBr, tail);
        esc.insert(esc.end(), escElse.begin(), escElse.end());

        if (isEmpty(iff.thenBr) && !isEmpty(iff.elseBr)) {
          iff.thenBr = std::move(iff.elseBr);
          iff.elseBr = nullptr;
          iff.negate = !iff.negate;
        } else if (isEmpty(iff.elseBr)) {
          iff.elseBr = nullptr;
        }
        return esc;
      }

      std::vector<Escape> lowerLoop(NodePtr &n) {
        auto &loop = std::get<ControlTree::Loop>(n->v);
        const int loopId = loop.loopId;
        auto esc = lowerNode(loop.body, /*tail=*/true);

        tryCondLoopPeephole(n);
        tryDoWhilePeephole(n);
        tryRotateLoopPeephole(n);

        // Unwind escapes crossing this loop: each owes one cascade
        // right after the loop construct.
        std::vector<NodePtr> after;
        std::vector<Escape> pending;
        for (auto &e: esc) {
          switch (e.kind) {
            case Escape::Kind::Break:
              if (e.remaining > 1) {
                after.push_back(make(ControlTree::FlagBreak{e.flag, false}));
                pending.push_back({e.flag, e.remaining - 1, e.kind, e.target});
              } else {
                after.push_back(make(ControlTree::FlagBreak{e.flag, true}));
              }
              break;
            case Escape::Kind::Continue:
              if (e.remaining > 1) {
                after.push_back(make(ControlTree::FlagBreak{e.flag, false}));
                pending.push_back({e.flag, e.remaining - 1, e.kind, e.target});
              } else {
                after.push_back(make(ControlTree::FlagContinue{e.flag}));
              }
              break;
            case Escape::Kind::Join:
              if (e.remaining > 0) {
                after.push_back(make(ControlTree::FlagBreak{e.flag, false}));
                pending.push_back({e.flag, e.remaining - 1, e.kind, e.target});
              } else {
                pending.push_back(e); // guard phase in the parent Seq
              }
              break;
          }
        }
        (void) loopId;
        if (!after.empty()) {
          ControlTree::Seq seq;
          seq.items.push_back(std::move(n));
          for (auto &a: after)
            seq.items.push_back(std::move(a));
          n = make(std::move(seq));
        }
        return pending;
      }

      // Loop{ body = [BlockStmts h (no instrs), If{h, T, E}] } where one
      // arm is exactly `break levels=1` → CondLoop ("while cond:").
      void tryCondLoopPeephole(NodePtr &n) {
        auto &loop = std::get<ControlTree::Loop>(n->v);
        if (!loop.body)
          return;
        auto *seq = std::get_if<ControlTree::Seq>(&loop.body->v);
        if (!seq || seq->items.size() != 2 || !seq->items[0] || !seq->items[1])
          return;
        auto *bs = std::get_if<ControlTree::BlockStmts>(&seq->items[0]->v);
        auto *iff = std::get_if<ControlTree::If>(&seq->items[1]->v);
        if (!bs || !iff || bs->block != loop.header || iff->block != loop.header)
          return;
        if (!f_.blocks[loop.header].instrs.empty())
          return;
        // "if (negate? !c : c) then T else E": the loop keeps running
        // while the non-break arm is taken.
        if (isSingleLevelBreak(iff->elseBr)) {
          NodePtr body = std::move(iff->thenBr);
          if (!body)
            body = make(ControlTree::Seq{});
          n = make(ControlTree::CondLoop{loop.loopId, loop.header, iff->negate, std::move(body)});
        } else if (isSingleLevelBreak(iff->thenBr)) {
          NodePtr body = std::move(iff->elseBr);
          if (!body)
            body = make(ControlTree::Seq{});
          n = make(ControlTree::CondLoop{loop.loopId, loop.header, !iff->negate, std::move(body)});
        }
      }

      // Loop{ body = Seq[..., If{L, then=Break lvl1, else=∅}] } whose
      // remaining body has no continue site binding to this loop →
      // DoWhile ("do { body } while (cond);"). The body always runs
      // once, then repeats while the break arm is *not* taken.
      void tryDoWhilePeephole(NodePtr &n) {
        auto *loop = std::get_if<ControlTree::Loop>(&n->v);
        if (!loop || !loop->body)
          return;
        auto *seq = std::get_if<ControlTree::Seq>(&loop->body->v);
        if (!seq || seq->items.empty() || !seq->items.back())
          return;
        auto *iff = std::get_if<ControlTree::If>(&seq->items.back()->v);
        if (!iff || iff->elseBr || !isSingleLevelBreak(iff->thenBr))
          return;
        for (std::size_t i = 0; i + 1 < seq->items.size(); ++i)
          if (hasLoopContinueSite(seq->items[i]))
            return;
        const std::size_t latch = iff->block;
        const bool negate = !iff->negate;
        seq->items.pop_back();
        n = make(ControlTree::DoWhile{loop->loopId, latch, negate, std::move(loop->body)});
      }

      // Loop{ body = Seq[BlockStmts H, If{H, R / pure Break lvl1}] }
      // where H *has* instructions (the empty-header case is CondLoop's,
      // handled above without duplication) → classic loop inversion:
      // hoist H before the loop and duplicate it at the body tail so
      // the condition becomes visible instead of `while True` + break:
      //   H; while (c) { R…; H }
      // The while-eligible shape is always exactly this two-item Seq:
      // the single in-loop arm target dominates every other in-loop
      // block, so all content nests inside the arm. Vetoed when R
      // contains a continue site binding to this loop — rotated,
      // `continue` would skip the trailing H and re-test early.
      void tryRotateLoopPeephole(NodePtr &n) {
        auto *loop = std::get_if<ControlTree::Loop>(&n->v);
        if (!loop || !loop->body)
          return;
        auto *seq = std::get_if<ControlTree::Seq>(&loop->body->v);
        if (!seq || seq->items.size() != 2 || !seq->items[0] || !seq->items[1])
          return;
        auto *bs = std::get_if<ControlTree::BlockStmts>(&seq->items[0]->v);
        auto *iff = std::get_if<ControlTree::If>(&seq->items[1]->v);
        if (!bs || !iff || bs->block != loop->header || iff->block != loop->header)
          return;
        NodePtr *rest;
        bool negate;
        if (isSingleLevelBreak(iff->elseBr)) {
          rest = &iff->thenBr;
          negate = iff->negate;
        } else if (isSingleLevelBreak(iff->thenBr)) {
          rest = &iff->elseBr;
          negate = !iff->negate;
        } else {
          return;
        }
        if (hasLoopContinueSite(*rest))
          return;
        ControlTree::Seq body;
        if (*rest && !isEmpty(*rest))
          body.items.push_back(std::move(*rest));
        body.items.push_back(make(ControlTree::BlockStmts{loop->header}));
        ControlTree::Seq outer;
        outer.items.push_back(std::move(seq->items[0]));
        outer.items.push_back(
            make(ControlTree::CondLoop{loop->loopId, loop->header, negate, make(std::move(body))})
        );
        n = make(std::move(outer));
      }

      ControlTree &tree_;
      const FunDecl &f_;
      const CFG &cfg_;
      std::unordered_map<std::string, int> flagIds_;
    };

  } // namespace

  ControlTree StructuredLowering::run(ControlTree tree, const FunDecl &f, const CFG &cfg) {
    Lowerer(tree, f, cfg).run();
    tree.lowered = true;
    return tree;
  }

} // namespace refractir
