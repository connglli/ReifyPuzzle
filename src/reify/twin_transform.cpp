#include "reify/twin_transform.hpp"

#include <map>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "analysis/type_utils.hpp"
#include "ast/ast.hpp"
#include "interp/type_layout.hpp"
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

    // `%dst = %left <op> %right` — the left operand is an id (coef), the
    // right an lvalue, matching OpAtom's shape. The type checker requires
    // both operands share bit-width, so callers keep them same-typed.
    Instr opInstr(
        const std::string &dst, const std::string &left, AtomOpKind op, const std::string &right
    ) {
      OpAtom o;
      o.op = op;
      o.coef = Coef{LocalOrSymId{LocalId{left, {}}}};
      o.rval = localLV(right);
      return assignInstr(dst, simpleExpr(Atom{std::move(o), {}}));
    }

    Instr andInstr(const std::string &acc, const std::string &cur) {
      return opInstr(acc, acc, AtomOpKind::And, cur);
    }

    // --- bijection guard: per-leaf bijective mixing -----------------------
    //
    // RefractIR arithmetic (`+ - * <<`) is strict signed: overflow is UB, so
    // the usual multiply/rotate mixers trap. But `x ↦ x ⊕ g(x)` is a
    // bijection on iW whenever every bit of g(x) depends only on strictly
    // higher bits of x (invert top-down: the MSB is unchanged, each lower
    // bit is then recovered from already-known higher bits). That admits two
    // overflow-free round shapes built only from logical shift `>>>`, `&`,
    // and `^`:
    //
    //   Xorshift(s):     x ^= x >>> s              (linear, s in [1,W-1])
    //   AndShift(a,b):   x ^= (x >>> a) & (x >>> b) (nonlinear, a,b in [1,W-1])
    //
    // Composing them yields a nonlinear bijection on iW, so comparing
    // mix(operand) against mix(expected) is exactly `operand == expected`
    // (zero collisions) while presenting an opaque `>>> & ^` surface with no
    // readable literal and no overflow hazard.

    struct MixRound {
      enum class Kind { Xorshift, AndShift } kind;
      std::uint64_t a;     // primary shift
      std::uint64_t b = 0; // secondary shift (AndShift only)
    };

    std::uint64_t widthMask(std::uint32_t w) { return w >= 64 ? ~0ull : ((1ull << w) - 1); }

    // Reinterpret a W-bit value as a signed iW literal (range
    // [-2^(W-1), 2^(W-1)-1], which the type checker enforces).
    std::int64_t signExtend(std::uint64_t v, std::uint32_t w) {
      if (w >= 64)
        return (std::int64_t) v;
      v &= widthMask(w);
      std::uint64_t sign = 1ull << (w - 1);
      if (v & sign)
        v |= ~widthMask(w); // set the high bits
      return (std::int64_t) v;
    }

    // The mixing rounds for width W, derived only from W so codegen and
    // constant evaluation agree. W <= 1 leaves no room for a shift in
    // [1,W-1], so the mix degenerates to the identity — still exact, just
    // unobfuscated (the caller keeps i1 on the direct-compare path).
    std::vector<MixRound> mixRounds(std::uint32_t w) {
      if (w <= 1)
        return {};
      auto s = [w](std::uint32_t v) -> std::uint64_t {
        if (v < 1)
          v = 1;
        if (v > w - 1)
          v = w - 1;
        return v;
      };
      return {
          {MixRound::Kind::Xorshift, s(w / 2)},
          {MixRound::Kind::AndShift, s(w / 3), s(2 * w / 3)},
          {MixRound::Kind::Xorshift, s(w / 4)},
          {MixRound::Kind::AndShift, s(w / 6 + 1), s(5 * w / 6)},
          {MixRound::Kind::Xorshift, s(3 * w / 5)},
      };
    }

    // Apply the same rounds in host arithmetic (masked to W bits) so the
    // guard's expected constant is bit-exact with what the emitted iW
    // instructions compute.
    std::uint64_t evalMix(const std::vector<MixRound> &rounds, std::uint64_t v, std::uint32_t w) {
      const std::uint64_t mask = widthMask(w);
      v &= mask;
      for (const auto &r: rounds) {
        if (r.kind == MixRound::Kind::Xorshift)
          v ^= (v >> r.a);
        else
          v ^= ((v >> r.a) & (v >> r.b));
        v &= mask;
      }
      return v;
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
    // Collect the base names an expression *reads* and whether it touches
    // memory (load / addr / ptr navigation; stores are flagged by the
    // instruction scan). Memory-op blocks are twin candidates: their effect
    // is the frame-state diff, and the full-state guard pins every value a
    // load can observe — planBlock therefore requires the WHOLE state to be
    // guardable for such blocks. Only non-intrinsic calls still reject: a
    // callee given a pointer into an outer frame could mutate state this
    // frame's diff does not see.

    struct ReadScan {
      std::unordered_set<std::string> reads;
      bool mem = false;
    };

    void scanIndices(const LValue &lv, ReadScan &rs) {
      for (const auto &acc: lv.accesses)
        if (auto ai = std::get_if<AccessIndex>(&acc))
          if (auto id = std::get_if<LocalOrSymId>(&ai->index))
            std::visit([&](auto &&v) { rs.reads.insert(v.name); }, *id);
    }

    void scanLV(const LValue &lv, ReadScan &rs) {
      rs.reads.insert(lv.base.name);
      scanIndices(lv, rs);
    }

    void scanCoef(const Coef &c, ReadScan &rs) {
      if (auto id = std::get_if<LocalOrSymId>(&c))
        std::visit([&](auto &&v) { rs.reads.insert(v.name); }, *id);
    }

    void scanSelectVal(const SelectVal &sv, ReadScan &rs) {
      if (auto rv = std::get_if<RValue>(&sv))
        scanLV(*rv, rs);
      else if (auto co = std::get_if<Coef>(&sv))
        scanCoef(*co, rs);
    }

    bool scanAtom(const Atom &a, ReadScan &rs);

    bool scanExpr(const Expr &e, ReadScan &rs) {
      if (!scanAtom(e.first, rs))
        return false;
      for (const auto &t: e.rest)
        if (!scanAtom(t.atom, rs))
          return false;
      return true;
    }

    bool scanCond(const Cond &c, ReadScan &rs) {
      return scanExpr(c.lhs, rs) && scanExpr(c.rhs, rs);
    }

    bool scanAtom(const Atom &a, ReadScan &rs) {
      return std::visit(
          [&](auto &&arg) -> bool {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, CoefAtom>) {
              scanCoef(arg.coef, rs);
              return true;
            } else if constexpr (std::is_same_v<T, RValueAtom>) {
              scanLV(arg.rval, rs);
              return true;
            } else if constexpr (std::is_same_v<T, OpAtom>) {
              scanCoef(arg.coef, rs);
              scanLV(arg.rval, rs);
              return true;
            } else if constexpr (std::is_same_v<T, UnaryAtom>) {
              scanLV(arg.rval, rs);
              return true;
            } else if constexpr (std::is_same_v<T, CmpAtom>) {
              scanSelectVal(arg.lhs, rs);
              scanSelectVal(arg.rhs, rs);
              return true;
            } else if constexpr (std::is_same_v<T, CastAtom>) {
              if (auto lv = std::get_if<LValue>(&arg.src))
                scanLV(*lv, rs);
              else if (auto si = std::get_if<SymId>(&arg.src))
                rs.reads.insert(si->name);
              return true;
            } else if constexpr (std::is_same_v<T, SelectAtom>) {
              if (arg.cond && !scanCond(*arg.cond, rs))
                return false;
              if (arg.maskExpr && !scanExpr(*arg.maskExpr, rs))
                return false;
              scanSelectVal(arg.vtrue, rs);
              scanSelectVal(arg.vfalse, rs);
              return true;
            } else if constexpr (std::is_same_v<T, CallAtom>) {
              // Intrinsic calls are pure value functions; only their
              // argument reads matter. A non-intrinsic call could have
              // out-of-frame effects, so reject it.
              if (!arg.resolvedIntrinsic)
                return false;
              for (const auto &e: arg.args)
                if (e && !scanExpr(*e, rs))
                  return false;
              return true;
            } else if constexpr (std::is_same_v<T, AddrAtom>) {
              rs.mem = true;
              scanIndices(arg.lv, rs); // the address itself is state-free
              return true;
            } else if constexpr (std::is_same_v<T, LoadAtom>) {
              rs.mem = true;
              scanLV(arg.rval, rs);
              return true;
            } else if constexpr (std::is_same_v<T, PtrIndexAtom>) {
              rs.mem = true;
              scanLV(arg.rval, rs);
              if (auto id = std::get_if<LocalOrSymId>(&arg.index))
                std::visit([&](auto &&v) { rs.reads.insert(v.name); }, *id);
              return true;
            } else {
              static_assert(std::is_same_v<T, PtrFieldAtom>);
              rs.mem = true;
              scanLV(arg.rval, rs);
              return true;
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
      // Ptr leaves only: the static `ptr T` type of the cell, and the
      // lvalue whose address reproduces the captured pointer (unset for
      // null pointers).
      TypePtr ptrType;
      std::optional<LValue> ptrTarget;

      LValue lvalue() const { return LValue{LocalId{root, {}}, path, {}}; }

      bool isPtr() const { return val.kind == StateValue::Kind::Ptr; }
    };

    // The RHS that reproduces a pointer leaf: `addr <target>` or `null`.
    Atom ptrRhsAtom(const LeafRef &leaf) {
      if (leaf.ptrTarget)
        return Atom{AddrAtom{*leaf.ptrTarget, {}}, {}};
      return coefAtom(Coef{NullLit{}});
    }

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
        Ptr,    // by-value ptr parameter, compared against an expected addr
      };
      std::string name;
      TypePtr type; // the root's static type in the entry function
      Kind kind;
      bool isParam = false;        // immutable in the entry function
      std::vector<LeafRef> leaves; // expected values from `s`
    };

    struct TwinPlan {
      std::vector<GuardRoot> guardRoots; // the entire guardable state, in
                                         // profile (name-sorted) order
      std::vector<LeafRef> defs;         // per-leaf constant reconstruction of s'
      std::vector<Instr> twinInstrs;     // solver-generated B' (empty = use defs)
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

    // Fill a ptr leaf's static type and reconstruction target. Returns
    // false when the leaf cannot be reproduced or guarded: unresolved
    // provenance, a target root that is missing / immutable / not
    // addressable, or an offset the static type cannot express as an
    // access path (e.g. one-past-the-end).
    bool fillPtrLeaf(
        LeafRef &leaf, const FunDecl &fn, const StructMap &structs, const TypeLayout &layout,
        const TypePtr &rootType
    ) {
      // Static type of the cell: walk the root type along the leaf path.
      TypePtr t = rootType;
      for (const auto &acc: leaf.path) {
        t = stepType(t, acc, structs);
        if (!t)
          return false;
      }
      if (!isPtrType(t))
        return false;
      leaf.ptrType = t;
      if (leaf.val.ptrNull)
        return true;
      if (leaf.val.ptrRoot.empty())
        return false; // opaque pointer — no way to reproduce it
      auto target = findRoot(fn, leaf.val.ptrRoot);
      if (!target || !target->isMutable)
        return false; // addr needs a mutable root
      auto path = ptrAccessPath(target->type, leaf.val.ptrOfs, pointeeType(t), layout);
      if (!path)
        return false;
      leaf.ptrTarget = LValue{LocalId{leaf.val.ptrRoot, {}}, std::move(*path), {}};
      return true;
    }

    // A block is a twin candidate iff its instructions are free of
    // non-intrinsic calls, its whole read set is guardable, and its effect
    // (the bit-exact state diff s -> s') is reproducible: scalar leaves as
    // literals, pointer leaves as `addr <target>` / `null`. Memory-op
    // blocks additionally require the ENTIRE state to be guardable — a
    // load can observe any root through a pointer. Fills `plan` with the
    // guard roots (all definitely-initialized state at block entry,
    // compared to `s`) and the def leaves (the diff, from `sPrime`).
    bool planBlock(
        const FunDecl &fn, const Block &b,
        const std::vector<std::pair<std::string, StateValue>> &sVars,
        const std::vector<std::pair<std::string, StateValue>> &sPrimeVars, const StateMap &s,
        const StructMap &structs, const TypeLayout &layout, TwinPlan &plan
    ) {
      // Roots whose whole value is assigned in the entry block are
      // definitely initialized in every OTHER block: the entry block is
      // straight-line and dominates the CFG. This admits the rysmith
      // pointer pattern (`let mut %p: ptr T = undef;` + `%p = addr ...`
      // in ^entry) into the guardable state.
      std::unordered_set<std::string> entryAssigned;
      if (!fn.blocks.empty() && fn.blocks.front().label.name != b.label.name)
        for (const auto &ins: fn.blocks.front().instrs)
          if (auto ai = std::get_if<AssignInstr>(&ins))
            if (ai->lhs.accesses.empty())
              entryAssigned.insert(ai->lhs.base.name);

      ReadScan rs;
      for (const auto &ins: b.instrs) {
        bool ok = std::visit(
            [&](auto &&i) -> bool {
              using T = std::decay_t<decltype(i)>;
              if constexpr (std::is_same_v<T, AssignInstr>) {
                if (!scanExpr(i.rhs, rs))
                  return false;
                scanIndices(i.lhs, rs); // indices are read; the base is written
                return true;
              } else if constexpr (std::is_same_v<T, AssumeInstr>) {
                return scanCond(i.cond, rs);
              } else if constexpr (std::is_same_v<T, RequireInstr>) {
                return scanCond(i.cond, rs);
              } else {
                static_assert(std::is_same_v<T, StoreInstr>);
                rs.mem = true;
                return scanExpr(i.ptr, rs) && scanExpr(i.val, rs);
              }
            },
            ins
        );
        if (!ok)
          return false;
      }

      // Guard roots: every definitely-initialized root of the entry state.
      // A root that cannot cross into the guard (undef leaves, opaque
      // pointers, immutable aggregate, vector nested in an aggregate) is
      // skipped when the block neither reads it nor touches memory —
      // soundness needs guard-set ⊇ read-set, and memory ops can read any
      // root — and rejects the block otherwise.
      std::unordered_set<std::string> guarded;
      for (const auto &[name, val]: sVars) {
        auto decl = findRoot(fn, name);
        bool guardable = decl.has_value() && (decl->initialized || entryAssigned.count(name) > 0);
        GuardRoot::Kind kind = GuardRoot::Kind::Scalar;
        if (guardable) {
          if (isPtrType(decl->type))
            kind = GuardRoot::Kind::Ptr;
          else if (std::holds_alternative<VecType>(decl->type->v))
            kind = GuardRoot::Kind::Vec;
          else if (TypeUtils::asArray(decl->type) || TypeUtils::asStruct(decl->type)) {
            kind = GuardRoot::Kind::Agg;
            // `addr %root` needs a mutable root; vector lanes inside an
            // aggregate cannot be reached through a pointer at all.
            guardable = decl->isMutable && !containsVec(decl->type, structs);
          }
        }
        std::vector<StateLeaf> leaves;
        if (guardable) {
          bool hasPtr = false, hasUndef = false;
          enumStateLeaves(val, leaves, hasPtr, hasUndef);
          guardable = !hasUndef && !leaves.empty();
        }
        GuardRoot root;
        if (guardable) {
          root = GuardRoot{name, decl->type, kind, decl->isParam, {}};
          for (auto &lf: leaves) {
            LeafRef ref{name, std::move(lf.path), lf.val, {}, {}};
            if (ref.isPtr() && !fillPtrLeaf(ref, fn, structs, layout, decl->type)) {
              guardable = false;
              break;
            }
            root.leaves.push_back(std::move(ref));
          }
        }
        if (!guardable) {
          if (rs.reads.count(name) || rs.mem)
            return false; // the block depends on state we cannot guard
          continue;
        }
        plan.guardRoots.push_back(std::move(root));
      }
      if (plan.guardRoots.empty())
        return false; // no live-in state to key the guard on

      // Defs: the bit-exact diff s -> s' over every root, including roots
      // that first become initialized inside the block. This subsumes
      // write-site analysis: store-through-pointer effects surface as
      // diffs of the pointee root.
      std::map<std::string, LeafRef> defMap;
      for (const auto &[name, val]: sPrimeVars) {
        auto decl = findRoot(fn, name);
        if (!decl)
          return false;
        std::vector<StateLeaf> leaves;
        bool hasPtr = false, hasUndef = false;
        enumStateLeaves(val, leaves, hasPtr, hasUndef);
        const StateValue *before = nullptr;
        if (auto it = s.find(name); it != s.end())
          before = it->second;
        for (auto &lf: leaves) {
          const StateValue *old = before ? navigate(*before, lf.path) : nullptr;
          if (old && bitExactEq(*old, lf.val))
            continue;
          if (decl->isParam || !decl->isMutable)
            return false; // an immutable root cannot have changed
          LeafRef ref{name, std::move(lf.path), lf.val, {}, {}};
          if (ref.isPtr() && !fillPtrLeaf(ref, fn, structs, layout, decl->type))
            return false;
          std::string key = leafKey(name, ref.path);
          defMap[key] = std::move(ref);
        }
      }
      if (defMap.empty())
        return false; // no effect to twin
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
    // on EVERY input, matched or not). With GuardStyle::Signature each
    // integer leaf is compared after a bijective mix, which is still exact
    // (see GuardStyle) but obfuscated.
    FunDecl buildGuardFun(
        const std::string &name, const TwinPlan &plan, const StructMap &structs, GuardStyle style
    ) {
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
        if (isPtrType(ty))
          return InitVal{InitVal::Kind::Undef, IntLit{0, {}}, {}};
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
      // After each root's main parameter(s) come one expected-pointer
      // parameter per ptr leaf (`%__e<n>`): the caller reconstructs the
      // expected pointer with `addr` / `null` and the guard compares with
      // `==`, which is defined even across objects.
      int eIdx = 0;
      for (const auto &root: plan.guardRoots) {
        switch (root.kind) {
          case GuardRoot::Kind::Scalar:
          case GuardRoot::Kind::Ptr:
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
        for (const auto &leaf: root.leaves)
          if (leaf.isPtr())
            g.params.push_back({LocalId{"%__e" + std::to_string(eIdx++), {}}, leaf.ptrType, {}});
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

      // Bijection guard: per-integer-type mixing scaffolding, built lazily
      // on first use of each type. Each type shares two temp scratch locals
      // and one const let per distinct shift amount. Keyed by type (not just
      // width) so the final `hash == const` compare stays same-typed.
      struct MixLets {
        std::string hash, t, u;                            // mutable iW scratch
        std::vector<MixRound> rounds;                      // empty for iW <= 1
        std::unordered_map<std::uint64_t, std::string> sh; // shift value -> const let
      };

      std::vector<std::pair<TypePtr, MixLets>> mixPool;
      auto getMix = [&](const TypePtr &ty, std::uint32_t w) -> MixLets & {
        for (auto &[mt, ml]: mixPool)
          if (TypeUtils::areTypesEqual(mt, ty))
            return ml;
        MixLets ml;
        ml.rounds = mixRounds(w);
        const std::string s = std::to_string(mixPool.size());
        ml.hash = "%__hh" + s;
        ml.t = "%__ht" + s;
        ml.u = "%__hu" + s;
        addLet(ml.hash, ty, intInit(0), /*mut=*/true);
        addLet(ml.t, ty, intInit(0), /*mut=*/true);
        addLet(ml.u, ty, intInit(0), /*mut=*/true);
        auto needShift = [&](std::uint64_t v) {
          if (ml.sh.count(v))
            return;
          std::string nm = "%__hs" + s + "_" + std::to_string(ml.sh.size());
          addLet(nm, ty, intInit(signExtend(v, w)), /*mut=*/false);
          ml.sh.emplace(v, std::move(nm));
        };
        for (const auto &r: ml.rounds) {
          needShift(r.a);
          if (r.kind == MixRound::Kind::AndShift)
            needShift(r.b);
        }
        return mixPool.emplace_back(ty, std::move(ml)).second;
      };

      Block e;
      e.label = BlockLabel{"^entry", {}};
      int kIdx = 0;
      eIdx = 0;
      for (const auto &root: plan.guardRoots) {
        for (const auto &leaf: root.leaves) {
          std::string operand;
          TypePtr leafT;
          switch (root.kind) {
            case GuardRoot::Kind::Scalar:
            case GuardRoot::Kind::Ptr:
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
          if (leaf.isPtr()) {
            // Pointer equality against the caller-reconstructed expected
            // pointer (defined across objects, so total on every input).
            e.instrs.push_back(cmpEqInstr("%__c", operand, "%__e" + std::to_string(eIdx++)));
          } else {
            // Bijection mode mixes integer leaves (width > 1) with a
            // nonlinear bijection before comparing against the pre-mixed
            // constant — still exact, just opaque. Float leaves and i1 have
            // no bijective integer primitive and stay direct.
            std::uint32_t w = 0;
            if (style == GuardStyle::Bijection)
              if (auto b = TypeUtils::getIntBitWidth(leafT))
                w = *b;
            std::string k = "%__k" + std::to_string(kIdx++);
            if (w > 1) {
              MixLets &ml = getMix(leafT, w);
              e.instrs.push_back(assignInstr(ml.hash, simpleExpr(rvalAtom(localLV(operand)))));
              for (const auto &r: ml.rounds) {
                if (r.kind == MixRound::Kind::Xorshift) {
                  // %h = %h ^ (%h >>> a)
                  e.instrs.push_back(opInstr(ml.t, ml.hash, AtomOpKind::LShr, ml.sh.at(r.a)));
                  e.instrs.push_back(opInstr(ml.hash, ml.hash, AtomOpKind::Xor, ml.t));
                } else {
                  // %h = %h ^ ((%h >>> a) & (%h >>> b))
                  e.instrs.push_back(opInstr(ml.t, ml.hash, AtomOpKind::LShr, ml.sh.at(r.a)));
                  e.instrs.push_back(opInstr(ml.u, ml.hash, AtomOpKind::LShr, ml.sh.at(r.b)));
                  e.instrs.push_back(opInstr(ml.t, ml.t, AtomOpKind::And, ml.u));
                  e.instrs.push_back(opInstr(ml.hash, ml.hash, AtomOpKind::Xor, ml.t));
                }
              }
              std::int64_t mixed =
                  signExtend(evalMix(ml.rounds, (std::uint64_t) leaf.val.intVal, w), w);
              addLet(k, leafT, intInit(mixed), /*mut=*/false);
              e.instrs.push_back(cmpEqInstr("%__c", ml.hash, k));
            } else {
              addLet(k, leafT, litInit(leaf.val), /*mut=*/false);
              e.instrs.push_back(cmpEqInstr("%__c", operand, k));
            }
          }
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
          case GuardRoot::Kind::Ptr:
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
        // Expected pointers, mirroring buildGuardFun's `%__e<n>` params.
        for (const auto &leaf: root.leaves)
          if (leaf.isPtr())
            args.push_back(std::make_shared<Expr>(simpleExpr(ptrRhsAtom(leaf))));
      }
      return args;
    }

    // Add `decls` to the program's intrinsic section, skipping declarations
    // it already has (same name, return type, and arity).
    void mergeIntrinsics(Program &prog, std::vector<IntrinsicDecl> &&decls) {
      for (auto &d: decls) {
        bool dup = false;
        for (const auto &e: prog.intrinsics)
          if (e.name.name == d.name.name && e.params.size() == d.params.size() &&
              TypeUtils::areTypesEqual(e.retType, d.retType)) {
            dup = true;
            break;
          }
        if (!dup)
          prog.intrinsics.push_back(std::move(d));
      }
    }

    class TwinTransform : public Transform {
    public:
      TwinTransform(double pTwin, TwinGenFn twinGen, GuardStyle guard) :
          pTwin_(pTwin), twinGen_(std::move(twinGen)), guard_(guard) {}

      std::string_view name() const override { return "TwinTransform"; }

      bool needsProfile() const override { return true; }

      TransformReport apply(Program &prog, TransformContext &ctx) override {
        TransformReport rep;
        std::uniform_real_distribution<double> coin(0.0, 1.0);

        StructMap structs;
        for (const auto &sd: prog.structs)
          structs[sd.name.name] = &sd;
        const TypeLayout layout(prog);

        for (auto &[pfKey, profile]: ctx.profiles) {
          // Group block-entry points by executing function. Sidecar files
          // that predate frame capture have no per-point function; those
          // traces are single-frame by construction (the leaf entry).
          std::unordered_map<std::string, std::vector<const StatePoint *>> byFn;
          std::vector<std::string> fnOrder;
          for (const auto &pt: profile.trace) {
            if (pt.instr != -1)
              continue;
            const std::string &fnName = pt.func.empty() ? profile.func : pt.func;
            auto [it, inserted] = byFn.try_emplace(fnName);
            if (inserted)
              fnOrder.push_back(fnName);
            it->second.push_back(&pt);
          }

          // Guard functions must be declared before their (sole) caller;
          // insert them after all functions are processed so the indices
          // stay stable while grafting.
          std::vector<std::pair<std::string, std::vector<FunDecl>>> pendingGuards;

          for (const auto &fnName: fnOrder) {
            FunDecl *fn = nullptr;
            for (auto &f: prog.funs)
              if (f.name.name == fnName) {
                fn = &f;
                break;
              }
            if (!fn)
              continue;

            std::unordered_map<std::string, const Block *> byLabel;
            for (const auto &b: fn->blocks)
              byLabel[b.label.name] = &b;

            const auto &pts = byFn[fnName];
            std::unordered_map<std::string, TwinPlan> decided;
            for (std::size_t t = 0; t + 1 < pts.size(); ++t) {
              // s' is the next point of the SAME activation: a point from
              // another frame (the caller resuming, a fresh call) says
              // nothing about this block's effect.
              if (pts[t]->frame != pts[t + 1]->frame)
                continue;
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
              if (!planBlock(
                      *fn, *bit->second, pts[t]->vars, pts[t + 1]->vars, s, structs, layout, plan
                  ))
                continue;
              if (coin(ctx.rng) >= pTwin_)
                continue;
              maybeGenerateTwin(prog, plan, s, sPrime, ctx);
              decided.emplace(label, std::move(plan));
            }
            if (decided.empty())
              continue;

            const std::string fnStem =
                fnName.empty() || fnName[0] != '@' ? fnName : fnName.substr(1);
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
              guardFuns.push_back(buildGuardFun(guardName, dit->second, structs, guard_));
              graft(b, dit->second, guardName, nb);
              ++rep.sites;
            }
            fn->blocks = std::move(nb);
            pendingGuards.emplace_back(fnName, std::move(guardFuns));
          }

          for (auto &[fnName, guards]: pendingGuards) {
            for (std::size_t i = 0; i < prog.funs.size(); ++i)
              if (prog.funs[i].name.name == fnName) {
                prog.funs.insert(
                    prog.funs.begin() + i, std::make_move_iterator(guards.begin()),
                    std::make_move_iterator(guards.end())
                );
                break;
              }
          }
        }
        return rep;
      }

    private:
      // Try the injected solver-backed generator for B'. The generated body
      // must reproduce s' for every guarded root, so it is only sound when
      // the guarded set covers every root the block writes (defs) and every
      // root has a target value in s'. On any miss, plan.twinInstrs stays
      // empty and graft falls back to constant reconstruction.
      void maybeGenerateTwin(
          Program &prog, TwinPlan &plan, const StateMap &s, const StateMap &sPrime,
          TransformContext &ctx
      ) {
        if (!twinGen_)
          return;
        std::unordered_set<std::string> guarded;
        for (const auto &r: plan.guardRoots)
          guarded.insert(r.name);
        for (const auto &d: plan.defs)
          if (!guarded.count(d.root))
            return;
        std::vector<TwinGenRoot> roots;
        roots.reserve(plan.guardRoots.size());
        for (const auto &r: plan.guardRoots) {
          auto si = s.find(r.name);
          auto ti = sPrime.find(r.name);
          if (si == s.end() || ti == sPrime.end())
            return;
          TwinGenRoot g{r.name, r.type, r.isParam, *si->second, *ti->second, {}};
          // Pointer cells: entry target from the guard leaves (state s),
          // exit target from the diff when the cell changed, else the same.
          for (const auto &leaf: r.leaves) {
            if (!leaf.isPtr())
              continue;
            TwinGenPtrFix fx{leaf.path, leaf.ptrType, leaf.ptrTarget, leaf.ptrTarget};
            const std::string key = leafKey(r.name, leaf.path);
            for (const auto &d: plan.defs)
              if (d.isPtr() && leafKey(d.root, d.path) == key) {
                fx.finalTarget = d.ptrTarget;
                break;
              }
            g.ptrFixes.push_back(std::move(fx));
          }
          roots.push_back(std::move(g));
        }
        if (auto res = twinGen_(prog, roots, ctx.rng)) {
          plan.twinInstrs = std::move(res->instrs);
          mergeIntrinsics(prog, std::move(res->intrinsics));
        }
      }

      // Expand block `b` into the guard / twin / orig / merge quartet. The
      // guard block is just a branch on the guard-function call.
      static void
      graft(Block &b, TwinPlan &plan, const std::string &guardName, std::vector<Block> &out) {
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
        if (!plan.twinInstrs.empty()) {
          twin.instrs = std::move(plan.twinInstrs);
        } else {
          for (const auto &d: plan.defs) {
            Atom rhs = d.isPtr() ? ptrRhsAtom(d) : coefAtom(litCoef(d.val));
            twin.instrs.push_back(assignLV(d.lvalue(), simpleExpr(std::move(rhs))));
          }
        }
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
      TwinGenFn twinGen_;
      GuardStyle guard_;
    };

  } // namespace

  std::unique_ptr<Transform> makeTwinTransform(double pTwin, TwinGenFn twinGen, GuardStyle guard) {
    return std::make_unique<TwinTransform>(pTwin, std::move(twinGen), guard);
  }

} // namespace refractir::reify
