#include "reify/twin_pass.hpp"

#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ast/ast.hpp"
#include "reify/state_profile.hpp"
#include "reify/type_gen.hpp"

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

    // A concrete scalar state value as an atom / coef literal.
    Coef litCoef(const StateValue &v) {
      if (v.kind == StateValue::Kind::Float)
        return Coef{FloatLit{v.floatVal, {}}};
      return Coef{IntLit{v.intVal, {}}};
    }

    Instr assignInstr(const std::string &lhs, Expr rhs) {
      return Instr{AssignInstr{localLV(lhs), std::move(rhs), {}}};
    }

    // `%dst = %var as <ty>` — a widening cast (lossless), used to bring a
    // guard variable to a canonical comparison width.
    Instr castInstr(const std::string &dst, const std::string &var, TypePtr ty) {
      CastAtom c;
      c.src = localLV(var);
      c.dstType = std::move(ty);
      return assignInstr(dst, simpleExpr(Atom{std::move(c), {}}));
    }

    // `%dst = cmp EQ %a, %b` — an i1 predicate over two same-width locals.
    Instr cmpEqInstr(const std::string &dst, const std::string &a, const std::string &b) {
      CmpAtom c;
      c.op = RelOp::EQ;
      c.lhs = SelectVal{RValue{localLV(a)}};
      c.rhs = SelectVal{RValue{localLV(b)}};
      return assignInstr(dst, simpleExpr(Atom{std::move(c), {}}));
    }

    // Append `%dst = (%var == k)` (i1) to `out`, normalizing both sides to a
    // canonical width so the CmpAtom's operands agree: a bare literal in a
    // cmp defaults to 32 bits and would mismatch a narrow/wide variable, so
    // we widen the variable (i64 / f64 — lossless) and load the literal into
    // a same-typed scratch (which gives the literal that width in the
    // assignment's type context). The equality is therefore exact.
    void emitEq(
        std::vector<Instr> &out, const std::string &dst, const std::string &var, const StateValue &k
    ) {
      if (k.kind == StateValue::Kind::Float) {
        out.push_back(castInstr("%__twfa", var, makeF64()));
        out.push_back(assignInstr("%__twfb", simpleExpr(coefAtom(litCoef(k)))));
        out.push_back(cmpEqInstr(dst, "%__twfa", "%__twfb"));
      } else {
        out.push_back(castInstr("%__twa", var, makeI64()));
        out.push_back(assignInstr("%__twb", simpleExpr(coefAtom(litCoef(k)))));
        out.push_back(cmpEqInstr(dst, "%__twa", "%__twb"));
      }
    }

    // `%acc = %acc & %cur` — fold one more predicate into the conjunction.
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

    // `br %guard != 0, ^then, ^else`
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

    // --- eligibility scan ------------------------------------------------

    void scanLV(const LValue &lv, std::unordered_set<std::string> &reads) {
      reads.insert(lv.base.name);
      for (const auto &acc: lv.accesses)
        if (auto ai = std::get_if<AccessIndex>(&acc))
          if (auto id = std::get_if<LocalOrSymId>(&ai->index))
            std::visit([&](auto &&v) { reads.insert(v.name); }, *id);
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
              if (arg.cond)
                if (!scanExpr(arg.cond->lhs, reads) || !scanExpr(arg.cond->rhs, reads))
                  return false;
              if (arg.maskExpr && !scanExpr(*arg.maskExpr, reads))
                return false;
              scanSelectVal(arg.vtrue, reads);
              scanSelectVal(arg.vfalse, reads);
              return true;
            } else {
              // AddrAtom / LoadAtom / PtrIndexAtom / PtrFieldAtom / CallAtom —
              // memory effects or calls: not a v1 twin candidate.
              return false;
            }
          },
          a.v
      );
    }

    // A block is a v1 twin candidate iff every instruction is an assignment
    // to a whole scalar local, with no memory / call effects, reading only
    // scalars. Fills `reads` (scalar names read) and `defs` (distinct
    // whole-scalar locals written, first-appearance order).
    bool blockEligible(
        const Block &b, const std::unordered_map<std::string, TypePtr> &vt,
        std::unordered_set<std::string> &reads, std::vector<std::string> &defs
    ) {
      std::unordered_set<std::string> defSeen;
      for (const auto &ins: b.instrs) {
        const auto *ai = std::get_if<AssignInstr>(&ins);
        if (!ai)
          return false; // assume / require / store
        if (!ai->lhs.accesses.empty())
          return false; // sub-lvalue write (vec lane / field / index)
        auto it = vt.find(ai->lhs.base.name);
        if (it == vt.end() || !isScalarType(it->second))
          return false;
        if (!scanExpr(ai->rhs, reads))
          return false;
        if (!defSeen.count(ai->lhs.base.name)) {
          defSeen.insert(ai->lhs.base.name);
          defs.push_back(ai->lhs.base.name);
        }
      }
      if (defs.empty())
        return false;
      for (const auto &r: reads) {
        auto it = vt.find(r);
        if (it == vt.end() || !isScalarType(it->second))
          return false;
      }
      return true;
    }

    // A planned graft for one block.
    struct TwinPlan {
      // (var, value-at-B-entry) equality checks forming the guard.
      std::vector<std::pair<std::string, StateValue>> guard;
      // (var, value-at-B-exit) constant assignments forming B'.
      std::vector<std::pair<std::string, StateValue>> defs;
    };

    const StateValue *
    findVar(const std::vector<std::pair<std::string, StateValue>> &vars, const std::string &name) {
      for (const auto &kv: vars)
        if (kv.first == name)
          return &kv.second;
      return nullptr;
    }

    bool isScalarStateValue(const StateValue &v) {
      return v.kind == StateValue::Kind::Int || v.kind == StateValue::Kind::Float;
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

          std::unordered_map<std::string, TypePtr> vt;
          for (const auto &p: fn->params)
            vt[p.name.name] = p.type;
          for (const auto &l: fn->lets)
            vt[l.name.name] = l.type;

          std::unordered_map<std::string, const Block *> byLabel;
          for (const auto &b: fn->blocks)
            byLabel[b.label.name] = &b;

          // Block-entry points, in execution order.
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
            const Block &b = *bit->second;

            std::unordered_set<std::string> reads;
            std::vector<std::string> defs;
            if (!blockEligible(b, vt, reads, defs))
              continue;

            // Guard = equality on each scalar the block reads (the live-in
            // state it depends on), in the profile's stable name order.
            TwinPlan plan;
            for (const auto &kv: pts[t]->vars) {
              if (!isScalarStateValue(kv.second))
                continue;
              if (reads.count(kv.first))
                plan.guard.emplace_back(kv.first, kv.second);
            }
            if (plan.guard.empty())
              continue; // no live-in scalars to key on

            // B' reconstructs each written scalar's exit value.
            bool defsOk = true;
            for (const auto &d: defs) {
              const StateValue *sv = findVar(pts[t + 1]->vars, d);
              if (!sv || !isScalarStateValue(*sv)) {
                defsOk = false;
                break;
              }
              plan.defs.emplace_back(d, *sv);
            }
            if (!defsOk)
              continue;

            if (coin(ctx.rng) >= pTwin_)
              continue;
            decided.emplace(label, std::move(plan));
          }

          if (decided.empty())
            continue;

          // Two i1 scratch locals suffice for every guard: each guard block
          // fully computes %__twg before branching, so their live ranges
          // never overlap across twins.
          auto addScratch = [&](const std::string &nm, TypePtr ty, bool isFloat) {
            for (const auto &l: fn->lets)
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
            fn->lets.push_back(std::move(d));
          };
          bool needInt = false, needFloat = false, needTwc = false;
          for (const auto &[lbl, plan]: decided) {
            if (plan.guard.size() > 1)
              needTwc = true;
            for (const auto &[var, val]: plan.guard) {
              if (val.kind == StateValue::Kind::Float)
                needFloat = true;
              else
                needInt = true;
            }
          }
          addScratch("%__twg", makeI1(), false); // conjunction accumulator (i1)
          if (needTwc)
            addScratch("%__twc", makeI1(), false); // current predicate (i1)
          if (needInt) {
            addScratch("%__twa", makeI64(), false); // widened int operand
            addScratch("%__twb", makeI64(), false); // int literal at i64
          }
          if (needFloat) {
            addScratch("%__twfa", makeF64(), true); // widened float operand
            addScratch("%__twfb", makeF64(), true); // float literal at f64
          }

          // Rebuild the block list, expanding each decided block into the
          // guard / twin / orig / merge quartet.
          std::vector<Block> nb;
          nb.reserve(fn->blocks.size() + 3 * decided.size());
          for (auto &b: fn->blocks) {
            auto dit = decided.find(b.label.name);
            if (dit == decided.end()) {
              nb.push_back(std::move(b));
              continue;
            }
            const TwinPlan &plan = dit->second;
            const std::string base = b.label.name;
            const std::string twinL = base + "__twin";
            const std::string origL = base + "__orig";
            const std::string mergeL = base + "__merge";

            // Guard block keeps the original label so predecessors are
            // untouched: compute the conjunction, then branch.
            Block guard;
            guard.label = BlockLabel{base, {}};
            for (std::size_t i = 0; i < plan.guard.size(); ++i) {
              const auto &[var, val] = plan.guard[i];
              if (i == 0) {
                emitEq(guard.instrs, "%__twg", var, val);
              } else {
                emitEq(guard.instrs, "%__twc", var, val);
                guard.instrs.push_back(andInstr("%__twg", "%__twc"));
              }
            }
            guard.term = brIf("%__twg", twinL, origL);

            // Twin block: B' constant reconstruction, then to merge.
            Block twin;
            twin.label = BlockLabel{twinL, {}};
            for (const auto &[var, val]: plan.defs)
              twin.instrs.push_back(assignInstr(var, simpleExpr(coefAtom(litCoef(val)))));
            twin.term = brTo(mergeL);

            // Orig block: the original body, then to merge.
            Block orig;
            orig.label = BlockLabel{origL, {}};
            orig.instrs = std::move(b.instrs);
            orig.term = brTo(mergeL);

            // Merge block: the original terminator (moved, not cloned).
            Block merge;
            merge.label = BlockLabel{mergeL, {}};
            merge.term = std::move(b.term);

            nb.push_back(std::move(guard));
            nb.push_back(std::move(twin));
            nb.push_back(std::move(orig));
            nb.push_back(std::move(merge));
            ++rep.sites;
          }
          fn->blocks = std::move(nb);
        }
        return rep;
      }

    private:
      double pTwin_;
      TwinGuard guard_;
    };

  } // namespace

  std::unique_ptr<Pass> makeTwinPass(double pTwin, TwinGuard guard) {
    return std::make_unique<TwinPass>(pTwin, guard);
  }

} // namespace refractir::reify
