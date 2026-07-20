#include "reify/twin_pass.hpp"

#include <map>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "analysis/type_utils.hpp"
#include "ast/ast.hpp"
#include "reify/state_profile.hpp"
#include "reify/type_gen.hpp"

namespace refractir::reify {

  namespace {

    // --- small AST builders (mirroring func_gen's helpers) ---------------

    TypePtr makeI1() {
      return std::make_shared<Type>(Type{IntType{IntType::Kind::ICustom, 1, {}}, {}});
    }

    TypePtr makePtr(TypePtr pointee) {
      return std::make_shared<Type>(Type{PtrType{std::move(pointee), {}}, {}});
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

    // `%dst = cmp == %a, %b` — both operands are same-typed locals, so the
    // equality is exact without any width coercion.
    Instr cmpEqInstr(const std::string &dst, const std::string &a, const std::string &b) {
      CmpAtom c;
      c.op = RelOp::EQ;
      c.lhs = SelectVal{RValue{localLV(a)}};
      c.rhs = SelectVal{RValue{localLV(b)}};
      return assignInstr(dst, simpleExpr(Atom{std::move(c), {}}));
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

    // `br <cond-expr> != 0, ^then, ^else` — the guard call site.
    Terminator brIfExpr(Expr cond, const std::string &thenL, const std::string &elseL) {
      Cond c;
      c.lhs = std::move(cond);
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

    // --- static-type walking ---------------------------------------------

    using StructMap = std::unordered_map<std::string, const StructDecl *>;

    // The static type reached from `t` after one access step. Returns nullptr
    // when the shape doesn't match (unknown struct/field, non-aggregate).
    TypePtr stepType(const TypePtr &t, const Access &acc, const StructMap &structs) {
      if (auto af = std::get_if<AccessField>(&acc)) {
        const StructType *st = TypeUtils::asStruct(t);
        if (!st)
          return nullptr;
        auto it = structs.find(st->name.name);
        if (it == structs.end())
          return nullptr;
        for (const auto &f: it->second->fields)
          if (f.name == af->field)
            return f.type;
        return nullptr;
      }
      if (const ArrayType *at = TypeUtils::asArray(t))
        return at->elem;
      if (t && std::holds_alternative<VecType>(t->v))
        return std::get<VecType>(t->v).elem;
      return nullptr;
    }

    // Whether an aggregate type contains a vector anywhere. Vector lanes are
    // not addressable, so such a root cannot be navigated through a pointer
    // parameter (rysmith never nests vectors in aggregates; hand-written
    // programs might).
    bool containsVec(const TypePtr &t, const StructMap &structs) {
      if (!t)
        return false;
      if (std::holds_alternative<VecType>(t->v))
        return true;
      if (const ArrayType *at = TypeUtils::asArray(t))
        return containsVec(at->elem, structs);
      if (const StructType *st = TypeUtils::asStruct(t)) {
        auto it = structs.find(st->name.name);
        if (it == structs.end())
          return false;
        for (const auto &f: it->second->fields)
          if (containsVec(f.type, structs))
            return true;
      }
      return false;
    }

    // --- twin planning ----------------------------------------------------

    // One state root the guard consumes, with its crossing strategy.
    struct GuardRoot {
      enum class Kind {
        Scalar, // by-value parameter of the root's own type
        Vec,    // one by-value parameter per lane
        Agg,    // by-address parameter (ptr [N] T / ptr @S), navigated inside
      };
      std::string name;
      TypePtr type; // the root's static type in the entry function
      Kind kind;
      std::vector<LeafRef> leaves; // expected values from `s`
    };

    struct TwinPlan {
      std::vector<GuardRoot> guardRoots; // the entire guardable state, in
                                         // profile (name-sorted) order
      std::vector<LeafRef> defs;         // per-leaf constant reconstruction of s'
    };

    // Locate a root's declaration in the entry function.
    struct RootDecl {
      TypePtr type;
      bool isParam = false;
      bool isMutable = false;
      bool initialized = false; // param, or let with a non-undef declared init
    };

    std::optional<RootDecl> findRoot(const FunDecl &fn, const std::string &name) {
      for (const auto &p: fn.params)
        if (p.name.name == name)
          return RootDecl{p.type, true, false, true};
      for (const auto &l: fn.lets)
        if (l.name.name == name)
          return RootDecl{
              l.type, false, l.isMutable, l.init.has_value() && l.init->kind != InitVal::Kind::Undef
          };
      return std::nullopt;
    }

    // A block is a twin candidate iff it has no store, its assignment RHSs and
    // any assume/require conditions are free of memory/call atoms, every
    // scalar leaf it reads or writes is concrete (no pointer / undef), and its
    // whole read set is guardable. Fills `plan` with the guard roots (the
    // ENTIRE definitely-initialized state at block entry, compared to `s`)
    // and the def leaves (exactly the leaves the block writes, reconstructed
    // from `sPrime`).
    bool planBlock(
        const FunDecl &fn, const Block &b,
        const std::vector<std::pair<std::string, StateValue>> &sVars, const StateMap &s,
        const StateMap &sPrime, const StructMap &structs, TwinPlan &plan
    ) {
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

      // Guard roots: every definitely-initialized root of the entry state.
      // A root that cannot cross into the guard (pointer-typed, undef or
      // pointer leaves, immutable aggregate, vector nested in an aggregate)
      // is skipped when the block does not read it — soundness only needs
      // guard-set ⊇ read-set — and rejects the block when it does.
      std::unordered_set<std::string> guarded;
      for (const auto &[name, val]: sVars) {
        auto decl = findRoot(fn, name);
        bool guardable = decl.has_value() && decl->initialized && !isPtrType(decl->type);
        GuardRoot::Kind kind = GuardRoot::Kind::Scalar;
        if (guardable) {
          if (std::holds_alternative<VecType>(decl->type->v))
            kind = GuardRoot::Kind::Vec;
          else if (TypeUtils::asArray(decl->type) || TypeUtils::asStruct(decl->type)) {
            kind = GuardRoot::Kind::Agg;
            // `addr %root` needs a mutable root; vector lanes inside an
            // aggregate cannot be reached through a pointer at all.
            guardable = decl->isMutable && !containsVec(decl->type, structs);
          }
        }
        std::vector<Leaf> leaves;
        if (guardable) {
          std::vector<Access> path;
          bool hasPtr = false, hasUndef = false;
          enumLeaves(val, path, leaves, hasPtr, hasUndef);
          guardable = !hasPtr && !hasUndef && !leaves.empty();
        }
        if (!guardable) {
          if (reads.count(name))
            return false; // the block depends on state we cannot guard
          continue;
        }
        GuardRoot root{name, decl->type, kind, {}};
        for (auto &lf: leaves)
          root.leaves.push_back({name, std::move(lf.path), lf.val});
        plan.guardRoots.push_back(std::move(root));
        guarded.insert(name);
      }
      if (plan.guardRoots.empty())
        return false; // no live-in state to key the guard on

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

    // --- guard-function synthesis ------------------------------------------

    std::string vecLaneParam(const std::string &root, std::int64_t lane) {
      return root + "__l" + std::to_string(lane);
    }

    std::int64_t laneOf(const LeafRef &leaf) {
      return std::get<IntLit>(std::get<AccessIndex>(leaf.path.front()).index).value;
    }

    // Build `fun @<name>(<state>) : i1` — a total, collision-free equality
    // check of the crossing state against the plan's expected values. Scalars
    // arrive by value, vectors per-lane, aggregates by address (navigated
    // with in-bounds ptrindex/ptrfield chains + load, so the body is UB-free
    // on EVERY input, matched or not).
    FunDecl buildGuardFun(const std::string &name, const TwinPlan &plan, const StructMap &structs) {
      FunDecl g;
      g.name = GlobalId{name, {}};
      g.retType = makeI1();

      auto addLet = [&g](const std::string &nm, TypePtr ty, InitVal iv, bool mut) {
        LetDecl d;
        d.isMutable = mut;
        d.name = LocalId{nm, {}};
        d.type = std::move(ty);
        d.init = std::move(iv);
        g.lets.push_back(std::move(d));
      };
      auto intInit = [](std::int64_t v) { return InitVal{InitVal::Kind::Int, IntLit{v, {}}, {}}; };
      auto zeroInit = [&](const TypePtr &ty) {
        if (TypeUtils::getFloatBitWidth(ty))
          return InitVal{InitVal::Kind::Float, FloatLit{0.0, {}}, {}};
        return intInit(0);
      };
      auto litInit = [&](const StateValue &v) {
        if (v.kind == StateValue::Kind::Float)
          return InitVal{InitVal::Kind::Float, FloatLit{v.floatVal, {}}, {}};
        return intInit(v.intVal);
      };

      // Params, in guard-root order (the caller emits args the same way).
      for (const auto &root: plan.guardRoots) {
        switch (root.kind) {
          case GuardRoot::Kind::Scalar:
            g.params.push_back({LocalId{root.name, {}}, root.type, {}});
            break;
          case GuardRoot::Kind::Vec: {
            const auto &vt = std::get<VecType>(root.type->v);
            for (const auto &leaf: root.leaves)
              g.params.push_back({LocalId{vecLaneParam(root.name, laneOf(leaf)), {}}, vt.elem, {}});
            break;
          }
          case GuardRoot::Kind::Agg:
            g.params.push_back({LocalId{root.name, {}}, makePtr(root.type), {}});
            break;
        }
      }

      // i1 is a signed 1-bit type: true is all-ones (-1), so the neutral
      // AND accumulator starts at -1.
      addLet("%__acc", makeI1(), intInit(-1), /*mut=*/true);
      addLet("%__c", makeI1(), intInit(0), /*mut=*/true);

      // Navigation / load scratch locals, one per distinct type.
      std::vector<std::pair<TypePtr, std::string>> ptrScratch, loadScratch;
      auto getScratch = [&](std::vector<std::pair<TypePtr, std::string>> &pool, const TypePtr &ty,
                            const char *prefix, bool isPtr) {
        for (const auto &[t, nm]: pool)
          if (TypeUtils::areTypesEqual(t, ty))
            return nm;
        std::string nm = std::string(prefix) + std::to_string(pool.size());
        pool.emplace_back(ty, nm);
        InitVal iv;
        if (isPtr)
          iv = InitVal{InitVal::Kind::Undef, IntLit{0, {}}, {}};
        else
          iv = zeroInit(ty);
        addLet(nm, ty, std::move(iv), /*mut=*/true);
        return nm;
      };

      Block e;
      e.label = BlockLabel{"^entry", {}};
      int kIdx = 0;
      for (const auto &root: plan.guardRoots) {
        for (const auto &leaf: root.leaves) {
          std::string operand;
          TypePtr leafT;
          switch (root.kind) {
            case GuardRoot::Kind::Scalar:
              operand = root.name;
              leafT = root.type;
              break;
            case GuardRoot::Kind::Vec:
              operand = vecLaneParam(root.name, laneOf(leaf));
              leafT = std::get<VecType>(root.type->v).elem;
              break;
            case GuardRoot::Kind::Agg: {
              // ptrindex/ptrfield down to the leaf cell, then load it. The
              // constant indices come from the state tree, which mirrors the
              // static type, so every step is in-bounds on any input.
              std::string cur = root.name;
              TypePtr curT = root.type; // pointee of `cur`
              for (const auto &acc: leaf.path) {
                TypePtr nextT = stepType(curT, acc, structs);
                std::string nxt = getScratch(ptrScratch, makePtr(nextT), "%__p", true);
                Atom nav =
                    std::holds_alternative<AccessField>(acc)
                        ? Atom{PtrFieldAtom{localLV(cur), std::get<AccessField>(acc).field, {}}, {}}
                        : Atom{
                              PtrIndexAtom{
                                  localLV(cur),
                                  Index{std::get<IntLit>(std::get<AccessIndex>(acc).index)},
                                  {}
                              },
                              {}
                          };
                e.instrs.push_back(assignInstr(nxt, simpleExpr(std::move(nav))));
                cur = nxt;
                curT = nextT;
              }
              operand = getScratch(loadScratch, curT, "%__v", false);
              e.instrs.push_back(
                  assignInstr(operand, simpleExpr(Atom{LoadAtom{localLV(cur), {}}, {}}))
              );
              leafT = curT;
              break;
            }
          }
          std::string k = "%__k" + std::to_string(kIdx++);
          addLet(k, leafT, litInit(leaf.val), /*mut=*/false);
          e.instrs.push_back(cmpEqInstr("%__c", operand, k));
          e.instrs.push_back(andInstr("%__acc", "%__c"));
        }
      }
      e.term = Terminator{RetTerm{simpleExpr(rvalAtom(localLV("%__acc"))), {}}};
      g.blocks.push_back(std::move(e));
      return g;
    }

    // The caller-side argument list matching buildGuardFun's parameters.
    std::vector<std::shared_ptr<Expr>> buildGuardArgs(const TwinPlan &plan) {
      std::vector<std::shared_ptr<Expr>> args;
      for (const auto &root: plan.guardRoots) {
        switch (root.kind) {
          case GuardRoot::Kind::Scalar:
            args.push_back(std::make_shared<Expr>(simpleExpr(rvalAtom(localLV(root.name)))));
            break;
          case GuardRoot::Kind::Vec:
            for (const auto &leaf: root.leaves) {
              LValue lane = localLV(root.name);
              lane.accesses.push_back(leaf.path.front());
              args.push_back(std::make_shared<Expr>(simpleExpr(rvalAtom(std::move(lane)))));
            }
            break;
          case GuardRoot::Kind::Agg:
            args.push_back(
                std::make_shared<Expr>(simpleExpr(Atom{AddrAtom{localLV(root.name), {}}, {}}))
            );
            break;
        }
      }
      return args;
    }

    class TwinPass : public Pass {
    public:
      explicit TwinPass(double pTwin) : pTwin_(pTwin) {}

      std::string_view name() const override { return "TwinPass"; }

      bool needsProfile() const override { return true; }

      PassReport apply(Program &prog, PassCtx &ctx) override {
        PassReport rep;
        std::uniform_real_distribution<double> coin(0.0, 1.0);

        StructMap structs;
        for (const auto &sd: prog.structs)
          structs[sd.name.name] = &sd;

        for (auto &[fname, profile]: ctx.profiles) {
          std::size_t fnIdx = prog.funs.size();
          for (std::size_t i = 0; i < prog.funs.size(); ++i)
            if (prog.funs[i].name.name == fname) {
              fnIdx = i;
              break;
            }
          if (fnIdx == prog.funs.size())
            continue;
          FunDecl *fn = &prog.funs[fnIdx];

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
            // Never re-twin the residue of a previous rytwin run.
            if (label.find("__twin") != std::string::npos ||
                label.find("__orig") != std::string::npos ||
                label.find("__merge") != std::string::npos)
              continue;
            auto bit = byLabel.find(label);
            if (bit == byLabel.end())
              continue;

            StateMap s = toStateMap(pts[t]->vars);
            StateMap sPrime = toStateMap(pts[t + 1]->vars);
            TwinPlan plan;
            if (!planBlock(*fn, *bit->second, pts[t]->vars, s, sPrime, structs, plan))
              continue;
            if (coin(ctx.rng) >= pTwin_)
              continue;
            decided.emplace(label, std::move(plan));
          }
          if (decided.empty())
            continue;

          const std::string fnStem = fname.empty() || fname[0] != '@' ? fname : fname.substr(1);
          std::vector<FunDecl> guardFuns;
          std::vector<Block> nb;
          nb.reserve(fn->blocks.size() + 3 * decided.size());
          for (auto &b: fn->blocks) {
            auto dit = decided.find(b.label.name);
            if (dit == decided.end()) {
              nb.push_back(std::move(b));
              continue;
            }
            std::string labelStem = b.label.name;
            if (!labelStem.empty() && labelStem[0] == '^')
              labelStem.erase(0, 1);
            std::string guardName = "@__twg_" + fnStem + "_" + labelStem;
            guardFuns.push_back(buildGuardFun(guardName, dit->second, structs));
            graft(b, dit->second, guardName, nb);
            ++rep.sites;
          }
          fn->blocks = std::move(nb);

          // Callees must be declared before their call sites; put the guard
          // functions directly in front of the entry function. (This
          // invalidates `fn` — it is not used past this point.)
          prog.funs.insert(
              prog.funs.begin() + fnIdx, std::make_move_iterator(guardFuns.begin()),
              std::make_move_iterator(guardFuns.end())
          );
        }
        return rep;
      }

    private:
      // Expand block `b` into the guard / twin / orig / merge quartet. The
      // guard block is just a branch on the guard-function call.
      static void
      graft(Block &b, const TwinPlan &plan, const std::string &guardName, std::vector<Block> &out) {
        const std::string base = b.label.name;
        const std::string twinL = base + "__twin";
        const std::string origL = base + "__orig";
        const std::string mergeL = base + "__merge";

        Block guard;
        guard.label = BlockLabel{base, {}};
        CallAtom call;
        call.callee = GlobalId{guardName, {}};
        call.args = buildGuardArgs(plan);
        guard.term = brIfExpr(simpleExpr(Atom{std::move(call), {}}), twinL, origL);

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
    };

  } // namespace

  std::unique_ptr<Pass> makeTwinPass(double pTwin) { return std::make_unique<TwinPass>(pTwin); }

} // namespace refractir::reify
