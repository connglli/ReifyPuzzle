#include "reify/twin_pass.hpp"

#include <map>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ast/ast.hpp"
#include "reify/state_profile.hpp"

namespace refractir::reify {

  namespace {

    // --- small AST builders (mirroring func_gen's helpers) ---------------

    TypePtr makeI1() {
      return std::make_shared<Type>(Type{IntType{IntType::Kind::ICustom, 1, {}}, {}});
    }

    TypePtr makeI64() {
      return std::make_shared<Type>(Type{IntType{IntType::Kind::I64, {}, {}}, {}});
    }

    TypePtr makeF64() {
      return std::make_shared<Type>(Type{FloatType{FloatType::Kind::F64, {}}, {}});
    }

    LValue localLV(const std::string &name) { return LValue{LocalId{name, {}}, {}, {}}; }

    Atom coefAtom(Coef c) { return Atom{CoefAtom{std::move(c), {}}, {}}; }

    Atom rvalAtom(RValue rv) { return Atom{RValueAtom{std::move(rv), {}}, {}}; }

    Expr simpleExpr(Atom a) { return Expr{std::move(a), {}, {}}; }

    Coef litCoef(const StateValue &v) {
      if (v.kind == StateValue::Kind::Float)
        return Coef{FloatLit{v.floatVal, {}}};
      return Coef{IntLit{v.intVal, {}}};
    }

    Instr assignInstr(const std::string &lhs, Expr rhs) {
      return Instr{AssignInstr{localLV(lhs), std::move(rhs), {}}};
    }

    Instr assignLV(LValue lhs, Expr rhs) {
      return Instr{AssignInstr{std::move(lhs), std::move(rhs), {}}};
    }

    // `%dst = <lvalue> as <ty>` — widen a (possibly sub-lvalue) leaf to a
    // canonical width for comparison.
    Instr castLVInstr(const std::string &dst, LValue src, TypePtr ty) {
      CastAtom c;
      c.src = std::move(src);
      c.dstType = std::move(ty);
      return assignInstr(dst, simpleExpr(Atom{std::move(c), {}}));
    }

    Instr cmpEqInstr(const std::string &dst, const std::string &a, const std::string &b) {
      CmpAtom c;
      c.op = RelOp::EQ;
      c.lhs = SelectVal{RValue{localLV(a)}};
      c.rhs = SelectVal{RValue{localLV(b)}};
      return assignInstr(dst, simpleExpr(Atom{std::move(c), {}}));
    }

    // Append `%dst = (<leaf> == k)` (i1). Both sides are widened to a
    // canonical width (i64 / f64 — lossless) and the literal is loaded into a
    // same-typed scratch so the CmpAtom's operands agree; a bare literal in a
    // cmp otherwise defaults to 32 bits. The equality is therefore exact.
    void emitEq(std::vector<Instr> &out, const std::string &dst, LValue leaf, const StateValue &k) {
      if (k.kind == StateValue::Kind::Float) {
        out.push_back(castLVInstr("%__twfa", std::move(leaf), makeF64()));
        out.push_back(assignInstr("%__twfb", simpleExpr(coefAtom(litCoef(k)))));
        out.push_back(cmpEqInstr(dst, "%__twfa", "%__twfb"));
      } else {
        out.push_back(castLVInstr("%__twa", std::move(leaf), makeI64()));
        out.push_back(assignInstr("%__twb", simpleExpr(coefAtom(litCoef(k)))));
        out.push_back(cmpEqInstr(dst, "%__twa", "%__twb"));
      }
    }

    Instr andInstr(const std::string &acc, const std::string &cur) {
      OpAtom o;
      o.op = AtomOpKind::And;
      o.coef = Coef{LocalOrSymId{LocalId{acc, {}}}};
      o.rval = localLV(cur);
      return assignInstr(acc, simpleExpr(Atom{std::move(o), {}}));
    }

    Terminator brTo(const std::string &dest) {
      BrTerm b;
      b.dest = BlockLabel{dest, {}};
      b.thenLabel = b.dest;
      b.elseLabel = b.dest;
      b.isConditional = false;
      return Terminator{std::move(b)};
    }

    Terminator brIf(const std::string &guard, const std::string &thenL, const std::string &elseL) {
      Cond c;
      c.lhs = simpleExpr(rvalAtom(localLV(guard)));
      c.op = RelOp::NE;
      c.rhs = simpleExpr(coefAtom(Coef{IntLit{0, {}}}));
      BrTerm b;
      b.cond = std::move(c);
      b.thenLabel = BlockLabel{thenL, {}};
      b.elseLabel = BlockLabel{elseL, {}};
      b.dest = b.thenLabel;
      b.isConditional = true;
      return Terminator{std::move(b)};
    }

    // --- read-set scanning ----------------------------------------------
    //
    // Collect the base names an expression *reads* and reject any atom with a
    // memory effect or call: load / addr / ptr-navigation / call are not v1
    // twin candidates (they need pointer provenance we don't yet model). A
    // block that reads or writes a pointer is likewise rejected downstream,
    // when the profile value tree turns up a Ptr leaf.

    void scanIndices(const LValue &lv, std::unordered_set<std::string> &reads) {
      for (const auto &acc: lv.accesses)
        if (auto ai = std::get_if<AccessIndex>(&acc))
          if (auto id = std::get_if<LocalOrSymId>(&ai->index))
            std::visit([&](auto &&v) { reads.insert(v.name); }, *id);
    }

    void scanLV(const LValue &lv, std::unordered_set<std::string> &reads) {
      reads.insert(lv.base.name);
      scanIndices(lv, reads);
    }

    void scanCoef(const Coef &c, std::unordered_set<std::string> &reads) {
      if (auto id = std::get_if<LocalOrSymId>(&c))
        std::visit([&](auto &&v) { reads.insert(v.name); }, *id);
    }

    void scanSelectVal(const SelectVal &sv, std::unordered_set<std::string> &reads) {
      if (auto rv = std::get_if<RValue>(&sv))
        scanLV(*rv, reads);
      else if (auto co = std::get_if<Coef>(&sv))
        scanCoef(*co, reads);
    }

    bool scanAtom(const Atom &a, std::unordered_set<std::string> &reads);

    bool scanExpr(const Expr &e, std::unordered_set<std::string> &reads) {
      if (!scanAtom(e.first, reads))
        return false;
      for (const auto &t: e.rest)
        if (!scanAtom(t.atom, reads))
          return false;
      return true;
    }

    bool scanCond(const Cond &c, std::unordered_set<std::string> &reads) {
      return scanExpr(c.lhs, reads) && scanExpr(c.rhs, reads);
    }

    bool scanAtom(const Atom &a, std::unordered_set<std::string> &reads) {
      return std::visit(
          [&](auto &&arg) -> bool {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, CoefAtom>) {
              scanCoef(arg.coef, reads);
              return true;
            } else if constexpr (std::is_same_v<T, RValueAtom>) {
              scanLV(arg.rval, reads);
              return true;
            } else if constexpr (std::is_same_v<T, OpAtom>) {
              scanCoef(arg.coef, reads);
              scanLV(arg.rval, reads);
              return true;
            } else if constexpr (std::is_same_v<T, UnaryAtom>) {
              scanLV(arg.rval, reads);
              return true;
            } else if constexpr (std::is_same_v<T, CmpAtom>) {
              scanSelectVal(arg.lhs, reads);
              scanSelectVal(arg.rhs, reads);
              return true;
            } else if constexpr (std::is_same_v<T, CastAtom>) {
              if (auto lv = std::get_if<LValue>(&arg.src))
                scanLV(*lv, reads);
              else if (auto si = std::get_if<SymId>(&arg.src))
                reads.insert(si->name);
              return true;
            } else if constexpr (std::is_same_v<T, SelectAtom>) {
              if (arg.cond && !scanCond(*arg.cond, reads))
                return false;
              if (arg.maskExpr && !scanExpr(*arg.maskExpr, reads))
                return false;
              scanSelectVal(arg.vtrue, reads);
              scanSelectVal(arg.vfalse, reads);
              return true;
            } else if constexpr (std::is_same_v<T, CallAtom>) {
              // Intrinsic calls are pure value functions (no memory side
              // effects, no pointer arguments), so a call's result is just
              // another scalar the twin reconstructs — we only need to cover
              // its argument reads. A non-intrinsic call (resolvedIntrinsic
              // unset) could have effects, so reject it.
              if (!arg.resolvedIntrinsic)
                return false;
              for (const auto &e: arg.args)
                if (e && !scanExpr(*e, reads))
                  return false;
              return true;
            } else {
              // AddrAtom / LoadAtom / PtrIndexAtom / PtrFieldAtom
              return false;
            }
          },
          a.v
      );
    }

    // --- profile value-tree helpers -------------------------------------

    using StateMap = std::unordered_map<std::string, const StateValue *>;

    StateMap toStateMap(const std::vector<std::pair<std::string, StateValue>> &vars) {
      StateMap m;
      for (const auto &kv: vars)
        m[kv.first] = &kv.second;
      return m;
    }

    // Navigate a value tree along `accs` (concrete accesses). Returns nullptr
    // if the shape doesn't match (e.g. field/index missing).
    const StateValue *navigate(const StateValue &root, const std::vector<Access> &accs) {
      const StateValue *cur = &root;
      for (const auto &acc: accs) {
        if (auto af = std::get_if<AccessField>(&acc)) {
          if (cur->kind != StateValue::Kind::Struct)
            return nullptr;
          const StateValue *next = nullptr;
          for (const auto &f: cur->fields)
            if (f.first == af->field) {
              next = &f.second;
              break;
            }
          if (!next)
            return nullptr;
          cur = next;
        } else {
          const auto &ai = std::get<AccessIndex>(acc);
          auto il = std::get_if<IntLit>(&ai.index);
          if (!il)
            return nullptr; // must be concrete by now
          if (cur->kind != StateValue::Kind::Array && cur->kind != StateValue::Kind::Vec)
            return nullptr;
          if (il->value < 0 || (std::size_t) il->value >= cur->elems.size())
            return nullptr;
          cur = &cur->elems[il->value];
        }
      }
      return cur;
    }

    struct Leaf {
      std::vector<Access> path; // from the root
      StateValue val;
    };

    // Enumerate scalar leaves of `v`, appending each with its full access path
    // (starting from `path`). Flags whether a pointer / undef leaf was seen.
    void enumLeaves(
        const StateValue &v, std::vector<Access> &path, std::vector<Leaf> &out, bool &hasPtr,
        bool &hasUndef
    ) {
      switch (v.kind) {
        case StateValue::Kind::Int:
        case StateValue::Kind::Float:
          out.push_back({path, v});
          break;
        case StateValue::Kind::Array:
        case StateValue::Kind::Vec:
          for (std::size_t i = 0; i < v.elems.size(); ++i) {
            path.push_back(AccessIndex{Index{IntLit{(std::int64_t) i, {}}}, {}});
            enumLeaves(v.elems[i], path, out, hasPtr, hasUndef);
            path.pop_back();
          }
          break;
        case StateValue::Kind::Struct:
          for (const auto &f: v.fields) {
            path.push_back(AccessField{f.first, {}});
            enumLeaves(f.second, path, out, hasPtr, hasUndef);
            path.pop_back();
          }
          break;
        case StateValue::Kind::Ptr:
          hasPtr = true;
          break;
        case StateValue::Kind::Undef:
        default:
          hasUndef = true;
          break;
      }
    }

    // Resolve any variable indices in `accs` to concrete IntLit indices using
    // the entry state `s`. Returns false if an index var has no scalar-int
    // value in `s`.
    bool
    resolveAccesses(const std::vector<Access> &accs, const StateMap &s, std::vector<Access> &out) {
      for (const auto &acc: accs) {
        if (auto af = std::get_if<AccessField>(&acc)) {
          out.push_back(*af);
        } else {
          const auto &ai = std::get<AccessIndex>(acc);
          if (auto il = std::get_if<IntLit>(&ai.index)) {
            out.push_back(AccessIndex{Index{*il}, {}});
          } else {
            std::string nm;
            std::visit([&](auto &&v) { nm = v.name; }, std::get<LocalOrSymId>(ai.index));
            auto it = s.find(nm);
            if (it == s.end() || it->second->kind != StateValue::Kind::Int)
              return false;
            out.push_back(AccessIndex{Index{IntLit{it->second->intVal, {}}}, {}});
          }
        }
      }
      return true;
    }

    // Canonical key for a (root, path) leaf so repeated writes dedup.
    std::string leafKey(const std::string &root, const std::vector<Access> &path) {
      std::string k = root;
      for (const auto &acc: path) {
        if (auto af = std::get_if<AccessField>(&acc))
          k += "." + af->field;
        else
          k += "[" + std::to_string(std::get<IntLit>(std::get<AccessIndex>(acc).index).value) + "]";
      }
      return k;
    }

    struct LeafRef {
      std::string root;
      std::vector<Access> path;
      StateValue val;

      LValue lvalue() const { return LValue{LocalId{root, {}}, path, {}}; }
    };

    struct TwinPlan {
      std::vector<LeafRef> guard; // per-leaf equalities on the live-in state
      std::vector<LeafRef> defs;  // per-leaf constant reconstruction of s'
    };

    // A block is a twin candidate iff it has no store, its assignment RHSs and
    // any assume/require conditions are free of memory/call atoms, and every
    // scalar leaf it reads or writes is concrete (no pointer / undef). Fills
    // `plan` with the guard leaves (defined scalar leaves of every read root,
    // compared to `s`) and the def leaves (exactly the leaves the block
    // writes, reconstructed from `sPrime`).
    bool planBlock(const Block &b, const StateMap &s, const StateMap &sPrime, TwinPlan &plan) {
      std::unordered_set<std::string> reads;

      struct WriteSite {
        std::string root;
        std::vector<Access> accesses;
      };

      std::vector<WriteSite> writes;

      for (const auto &ins: b.instrs) {
        bool ok = std::visit(
            [&](auto &&i) -> bool {
              using T = std::decay_t<decltype(i)>;
              if constexpr (std::is_same_v<T, AssignInstr>) {
                if (!scanExpr(i.rhs, reads))
                  return false;
                scanIndices(i.lhs, reads); // indices are read; the base is written
                writes.push_back({i.lhs.base.name, i.lhs.accesses});
                return true;
              } else if constexpr (std::is_same_v<T, AssumeInstr>) {
                return scanCond(i.cond, reads);
              } else if constexpr (std::is_same_v<T, RequireInstr>) {
                return scanCond(i.cond, reads);
              } else {
                return false; // StoreInstr — memory effect
              }
            },
            ins
        );
        if (!ok)
          return false;
      }
      if (writes.empty())
        return false;

      // Guard: every defined scalar leaf of every read root, compared to s.
      for (const auto &root: reads) {
        auto it = s.find(root);
        if (it == s.end())
          continue; // not in the live-in state (e.g. written-before-read)
        std::vector<Leaf> leaves;
        std::vector<Access> path;
        bool hasPtr = false, hasUndef = false;
        enumLeaves(*it->second, path, leaves, hasPtr, hasUndef);
        if (hasPtr)
          return false; // reads a pointer — not modeled
        for (auto &lf: leaves)
          plan.guard.push_back({root, std::move(lf.path), lf.val});
      }
      if (plan.guard.empty())
        return false; // no live-in scalar to key the guard on

      // Defs: exactly the leaves each write touches, from s'.
      std::map<std::string, LeafRef> defMap;
      for (const auto &w: writes) {
        std::vector<Access> concrete;
        if (!resolveAccesses(w.accesses, s, concrete))
          return false;
        auto it = sPrime.find(w.root);
        if (it == sPrime.end())
          return false;
        const StateValue *sub = navigate(*it->second, concrete);
        if (!sub)
          return false;
        std::vector<Leaf> leaves;
        std::vector<Access> base = concrete;
        bool hasPtr = false, hasUndef = false;
        enumLeaves(*sub, base, leaves, hasPtr, hasUndef);
        if (hasPtr || hasUndef)
          return false; // can't reconstruct a pointer / undef leaf
        for (auto &lf: leaves) {
          // Compute the key before moving lf.path into the value — doing both
          // in one statement would read a moved-from path and collapse the
          // key to the bare root, silently dropping all but one leaf.
          std::string key = leafKey(w.root, lf.path);
          defMap[key] = {w.root, std::move(lf.path), lf.val};
        }
      }
      if (defMap.empty())
        return false;
      for (auto &[k, v]: defMap)
        plan.defs.push_back(std::move(v));
      return true;
    }

    class TwinPass : public Pass {
    public:
      TwinPass(double pTwin, TwinGuard guard) : pTwin_(pTwin), guard_(guard) {}

      std::string_view name() const override { return "TwinPass"; }

      bool needsProfile() const override { return true; }

      PassReport apply(Program &prog, PassCtx &ctx) override {
        PassReport rep;
        if (guard_ != TwinGuard::Exact) {
          rep.ok = false;
          rep.message = "only --guard exact is implemented in this version";
          return rep;
        }
        std::uniform_real_distribution<double> coin(0.0, 1.0);

        for (auto &[fname, profile]: ctx.profiles) {
          FunDecl *fn = nullptr;
          for (auto &f: prog.funs)
            if (f.name.name == fname) {
              fn = &f;
              break;
            }
          if (!fn)
            continue;

          std::unordered_map<std::string, const Block *> byLabel;
          for (const auto &b: fn->blocks)
            byLabel[b.label.name] = &b;

          std::vector<const StatePoint *> pts;
          for (const auto &pt: profile.trace)
            if (pt.instr == -1)
              pts.push_back(&pt);

          std::unordered_map<std::string, TwinPlan> decided;
          for (std::size_t t = 0; t + 1 < pts.size(); ++t) {
            const std::string &label = pts[t]->block;
            if (decided.count(label))
              continue; // first visit only
            auto bit = byLabel.find(label);
            if (bit == byLabel.end())
              continue;

            StateMap s = toStateMap(pts[t]->vars);
            StateMap sPrime = toStateMap(pts[t + 1]->vars);
            TwinPlan plan;
            if (!planBlock(*bit->second, s, sPrime, plan))
              continue;
            if (coin(ctx.rng) >= pTwin_)
              continue;
            decided.emplace(label, std::move(plan));
          }
          if (decided.empty())
            continue;

          addScratch(*fn, decided);

          std::vector<Block> nb;
          nb.reserve(fn->blocks.size() + 3 * decided.size());
          for (auto &b: fn->blocks) {
            auto dit = decided.find(b.label.name);
            if (dit == decided.end()) {
              nb.push_back(std::move(b));
              continue;
            }
            graft(b, dit->second, nb);
            ++rep.sites;
          }
          fn->blocks = std::move(nb);
        }
        return rep;
      }

    private:
      // Two i1 scratch locals plus the widening operands. Their live ranges
      // never overlap (each guard computes and consumes them before
      // branching), so one set serves every twin. Only the kinds actually
      // used are declared.
      static void
      addScratch(FunDecl &fn, const std::unordered_map<std::string, TwinPlan> &decided) {
        bool needInt = false, needFloat = false, needTwc = false;
        for (const auto &[lbl, plan]: decided) {
          if (plan.guard.size() > 1)
            needTwc = true;
          for (const auto &g: plan.guard)
            (g.val.kind == StateValue::Kind::Float ? needFloat : needInt) = true;
        }
        auto add = [&](const std::string &nm, TypePtr ty, bool isFloat) {
          for (const auto &l: fn.lets)
            if (l.name.name == nm)
              return;
          LetDecl d;
          d.isMutable = true;
          d.name = LocalId{nm, {}};
          d.type = std::move(ty);
          InitVal iv;
          if (isFloat) {
            iv.kind = InitVal::Kind::Float;
            iv.value = FloatLit{0.0, {}};
          } else {
            iv.kind = InitVal::Kind::Int;
            iv.value = IntLit{0, {}};
          }
          d.init = iv;
          fn.lets.push_back(std::move(d));
        };
        add("%__twg", makeI1(), false);
        if (needTwc)
          add("%__twc", makeI1(), false);
        if (needInt) {
          add("%__twa", makeI64(), false);
          add("%__twb", makeI64(), false);
        }
        if (needFloat) {
          add("%__twfa", makeF64(), true);
          add("%__twfb", makeF64(), true);
        }
      }

      // Expand block `b` into the guard / twin / orig / merge quartet.
      static void graft(Block &b, const TwinPlan &plan, std::vector<Block> &out) {
        const std::string base = b.label.name;
        const std::string twinL = base + "__twin";
        const std::string origL = base + "__orig";
        const std::string mergeL = base + "__merge";

        Block guard;
        guard.label = BlockLabel{base, {}};
        for (std::size_t i = 0; i < plan.guard.size(); ++i) {
          const std::string dst = i == 0 ? "%__twg" : "%__twc";
          emitEq(guard.instrs, dst, plan.guard[i].lvalue(), plan.guard[i].val);
          if (i > 0)
            guard.instrs.push_back(andInstr("%__twg", "%__twc"));
        }
        guard.term = brIf("%__twg", twinL, origL);

        Block twin;
        twin.label = BlockLabel{twinL, {}};
        for (const auto &d: plan.defs)
          twin.instrs.push_back(assignLV(d.lvalue(), simpleExpr(coefAtom(litCoef(d.val)))));
        twin.term = brTo(mergeL);

        Block orig;
        orig.label = BlockLabel{origL, {}};
        orig.instrs = std::move(b.instrs);
        orig.term = brTo(mergeL);

        Block merge;
        merge.label = BlockLabel{mergeL, {}};
        merge.term = std::move(b.term);

        out.push_back(std::move(guard));
        out.push_back(std::move(twin));
        out.push_back(std::move(orig));
        out.push_back(std::move(merge));
      }

      double pTwin_;
      TwinGuard guard_;
    };

  } // namespace

  std::unique_ptr<Pass> makeTwinPass(double pTwin, TwinGuard guard) {
    return std::make_unique<TwinPass>(pTwin, guard);
  }

} // namespace refractir::reify
