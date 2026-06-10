#include "reify/expr_gen.hpp"

#include <algorithm>
#include <cassert>
#include <optional>
#include "reify/hyperparameters.hpp"
#include "reify/intrinsic_whitelist.hpp"

namespace symir::reify {

  // ---------------------------------------------------------------------------
  // Internal helpers — AST factories
  // ---------------------------------------------------------------------------

  static TypePtr makeI32() {
    return std::make_shared<Type>(Type{IntType{IntType::Kind::I32, {}, {}}, {}});
  }

  static LValue localLV(const std::string &name) { return LValue{LocalId{name, {}}, {}, {}}; }

  static LValue arrayLV(const std::string &name, int64_t idx) {
    LValue lv;
    lv.base = LocalId{name, {}};
    lv.accesses.push_back(AccessIndex{Index{IntLit{idx, {}}}, {}});
    return lv;
  }

  static LValue structLV(const std::string &name, const std::string &field) {
    LValue lv;
    lv.base = LocalId{name, {}};
    lv.accesses.push_back(AccessField{field, {}});
    return lv;
  }

  static Coef symCoef(const std::string &sname) { return LocalOrSymId{SymId{sname, {}}}; }

  static Coef intCoef(int64_t v) { return IntLit{v, {}}; }

  static Coef floatCoef(double v) { return FloatLit{v, {}}; }

  static Atom coefAtom(Coef c) { return Atom{CoefAtom{std::move(c), {}}, {}}; }

  static Atom opAtom(AtomOpKind op, Coef c, RValue rv) {
    return Atom{OpAtom{op, std::move(c), std::move(rv), {}}, {}};
  }

  static Atom rvalAtom(RValue rv) { return Atom{RValueAtom{std::move(rv), {}}, {}}; }

  static Atom unaryAtom(RValue rv) {
    return Atom{UnaryAtom{UnaryOpKind::Not, std::move(rv), {}}, {}};
  }

  static Expr simpleExpr(Atom a) { return Expr{std::move(a), {}, {}}; }

  // Pick a random element from a non-empty vector
  template<typename T>
  static const T &pickOne(std::mt19937 &rng, const std::vector<T> &v) {
    assert(!v.empty());
    std::uniform_int_distribution<int> d(0, (int) v.size() - 1);
    return v[d(rng)];
  }

  // Filter a candidate var pool, dropping any entry whose name matches
  // `excludeName`. When `excludeName` is unset the input is returned
  // unchanged. Applied at every variable-pool pick in the body of an RHS
  // expression to make `%x = %x;`, `%x = -4 * %x;`, etc. impossible.
  static std::vector<const VarEntry *>
  excluding(std::vector<const VarEntry *> vec, const std::optional<std::string> &excludeName) {
    if (!excludeName)
      return vec;
    vec.erase(
        std::remove_if(
            vec.begin(), vec.end(), [&](const VarEntry *v) { return v->name == *excludeName; }
        ),
        vec.end()
    );
    return vec;
  }

  // A Coef carries no runtime dependency unless it names a LocalId (a
  // SymId resolves to a solver-chosen literal post-solve, a bare literal
  // is trivially constant).
  static bool isTriviallyConstantCoef(const Coef &c) {
    if (auto *lsi = std::get_if<LocalOrSymId>(&c))
      return std::holds_alternative<SymId>(*lsi); // LocalId reads the var → not trivial
    return true;                                  // IntLit, FloatLit, NullLit
  }

  static bool isTriviallyConstantAtom(const Atom &a); // mutually recursive with Expr

  // An Expr is trivially constant when every atom in it is.
  static bool isTriviallyConstantExpr(const Expr &e) {
    if (!isTriviallyConstantAtom(e.first))
      return false;
    for (const auto &t: e.rest)
      if (!isTriviallyConstantAtom(t.atom))
        return false;
    return true;
  }

  // A select arm (RValue | Coef): an RValue reads a local; a Coef follows
  // the Coef rule above.
  static bool isTriviallyConstantSelectVal(const SelectVal &sv) {
    if (std::holds_alternative<RValue>(sv))
      return false; // RValue == LValue → reads a local
    return isTriviallyConstantCoef(std::get<Coef>(sv));
  }

  // An atom is "trivially constant" when it carries no runtime LValue
  // dependency: a bare literal / SymId CoefAtom (the solver picks a literal
  // post-solve, indistinguishable from a hardcoded literal), a CastAtom
  // whose source is not an LValue, or a SelectAtom whose condition/mask and
  // both arms are themselves trivially constant (e.g. `select 0 == 0, 1, 2`
  // — which the compiler folds flat). Every other atom kind (RValueAtom,
  // OpAtom, UnaryAtom, CastAtom-from-LValue, AddrAtom, LoadAtom, PtrIndex,
  // PtrField, CallAtom, or a select reading any local) contributes a real
  // data dependency. Used by the post-check that rejects all-trivial RHSs.
  static bool isTriviallyConstantAtom(const Atom &a) {
    if (auto *ca = std::get_if<CoefAtom>(&a.v))
      return isTriviallyConstantCoef(ca->coef);
    if (auto *cast = std::get_if<CastAtom>(&a.v))
      return !std::holds_alternative<LValue>(cast->src);
    if (auto *sa = std::get_if<SelectAtom>(&a.v)) {
      if (!isTriviallyConstantSelectVal(sa->vtrue) || !isTriviallyConstantSelectVal(sa->vfalse))
        return false;
      if (sa->cond)
        return isTriviallyConstantExpr(sa->cond->lhs) && isTriviallyConstantExpr(sa->cond->rhs);
      if (sa->maskExpr)
        return isTriviallyConstantExpr(*sa->maskExpr);
      return true;
    }
    return false;
  }

  // ---------------------------------------------------------------------------
  // SymCounter implementation
  // ---------------------------------------------------------------------------

  std::string SymCounter::next(SymKind kind, TypePtr type) {
    auto name = "%?s" + std::to_string(n++);
    int64_t lo, hi;
    if (kind == SymKind::Coef) {
      lo = coefLo;
      hi = coefHi;
    } else if (kind == SymKind::Value) {
      lo = valueLo;
      hi = valueHi;
    } else {
      lo = indexLo;
      hi = indexHi;
    }
    entries.push_back({name, kind, std::move(type), lo, hi});
    return name;
  }

  std::string SymCounter::nextCoef(const TypePtr &type) { return next(SymKind::Coef, type); }

  std::string SymCounter::nextValue() { return next(SymKind::Value, makeI32()); }

  std::string SymCounter::nextIndex() { return next(SymKind::Index, makeI32()); }

  int SymCounter::countOfKind(SymKind kind) const {
    int c = 0;
    for (const auto &e: entries)
      if (e.kind == kind)
        c++;
    return c;
  }

  std::vector<std::string> SymCounter::namesOfKindSince(SymKind kind, int since) const {
    std::vector<std::string> result;
    int c = 0;
    for (const auto &e: entries) {
      if (e.kind == kind) {
        if (c >= since)
          result.push_back(e.name);
        c++;
      }
    }
    return result;
  }

  std::vector<SymDecl> SymCounter::makeDecls() const {
    std::vector<SymDecl> decls;
    for (const auto &e: entries) {
      SymDecl d;
      d.name = SymId{e.name, {}};
      d.kind = e.kind;
      d.type = e.type;
      d.domain = Domain{DomainInterval{e.lo, e.hi, {}}};
      decls.push_back(std::move(d));
    }
    return decls;
  }

  // ---------------------------------------------------------------------------
  // Interest coef requires
  // ---------------------------------------------------------------------------

  std::vector<Instr> interestCoefRequires(
      std::mt19937 &rng, const SymCounter &sym, int coefCountBefore, double pLargeCoef,
      int64_t largeCoefThreshold
  ) {
    std::vector<Instr> instrs;
    std::uniform_real_distribution<double> coin(0.0, 1.0);
    std::uniform_int_distribution<int> side(0, 1);
    // Walk new coef entries (post-coefCountBefore) directly so each
    // require's threshold can be sized to that coef's actual bitwidth —
    // a require like `c > 2^20` is malformed for an i8/i16 coef because
    // the literal doesn't fit the typechecker's per-type range.
    int idx = 0;
    for (const auto &e: sym.entries) {
      if (e.kind != SymKind::Coef)
        continue;
      const int thisIdx = idx++;
      if (thisIdx < coefCountBefore)
        continue;
      if (coin(rng) >= pLargeCoef)
        continue;
      // [bugfix] Vec lane operations create coef syms whose type is the
      // vec element type (expr_gen.cpp `sym->nextCoef(vt.elem)`), which
      // can be f32 / f64. intBitWidth asserts isIntType and would
      // crash. The "large coef" notion only applies to integer
      // bit-vectors, so skip non-int coefs entirely.
      if (!isIntType(e.type))
        continue;
      // Effective feasible range for this coef = its declared --coef-domain
      // [e.lo, e.hi] intersected with the type's representable range. The
      // solver applies the same clamp when emitting the domain constraint
      // (see solver.cpp), so the require literal must land inside this range
      // — otherwise it is either rejected by the typechecker (literal too
      // wide for the coef type) or unsatisfiable against the domain (the R5
      // UNSAT-under-narrow-`--coef-domain` bug).
      uint32_t bits = intBitWidth(e.type);
      if (bits == 0) // unknown — skip
        continue;
      int64_t typeLo, typeHi;
      if (bits >= 64) {
        typeLo = INT64_MIN;
        typeHi = INT64_MAX;
      } else {
        typeHi = ((int64_t) 1 << (bits - 1)) - 1;
        typeLo = -typeHi - 1;
      }
      int64_t dlo = std::max(e.lo, typeLo);
      int64_t dhi = std::min(e.hi, typeHi);
      if (dlo > dhi)
        continue; // empty domain — nothing to constrain

      // Clamp the requested magnitude threshold into the feasible range,
      // independently per side: `c > posT` needs a value in (posT, dhi];
      // `c < negT` needs one in [dlo, negT). When the domain is roomier than
      // the threshold the require keeps its requested magnitude; when it is
      // tighter the require degrades to the largest in-domain magnitude
      // (forcing c toward dhi / dlo) instead of going UNSAT.
      const int64_t T = largeCoefThreshold;
      int64_t posT = std::min(T, dhi - 1);  // c in (posT, dhi]
      int64_t negT = std::max(-T, dlo + 1); // c in [dlo, negT)
      bool posViable = dhi >= 1 && posT >= 0 && posT < dhi;
      bool negViable = dlo <= -1 && negT <= 0 && negT > dlo;
      if (!posViable && !negViable)
        continue; // domain too tight to force a nonzero magnitude

      // Encode |c| > T as a single relop by picking a viable sign.
      // RequireInstr.cond is one Cond, so we can't OR two predicates in a
      // single require; the disjunction `c > posT ∨ c < negT` is encoded
      // per-coef as one of the two sides. When both are viable the side is
      // random so the magnitude distribution covers both signs.
      bool positive = (posViable && negViable) ? (side(rng) == 0) : posViable;
      RequireInstr req;
      req.cond.lhs = simpleExpr(coefAtom(symCoef(e.name)));
      req.cond.op = positive ? RelOp::GT : RelOp::LT;
      req.cond.rhs = simpleExpr(coefAtom(intCoef(positive ? posT : negT)));
      req.message = positive ? "coef large positive" : "coef large negative";
      instrs.push_back(Instr{std::move(req)});
    }
    return instrs;
  }

  // ---------------------------------------------------------------------------
  // Core expression builders
  // ---------------------------------------------------------------------------

  // Return the [lo, hi] inclusive range used for concrete integer literals
  // of the given target type. Centralised so off-path coef draws and bare
  // literal atoms share a single source of truth (see R6).
  static std::pair<int64_t, int64_t> concreteIntRange(const TypePtr &targetType) {
    uint32_t bits = intBitWidth(targetType);
    int64_t lo = rysmith::hp::kConcreteInt_Default_Lo, hi = rysmith::hp::kConcreteInt_Default_Hi;
    if (bits == 8) {
      lo = rysmith::hp::kConcreteInt_I8_Lo;
      hi = rysmith::hp::kConcreteInt_I8_Hi;
    } else if (bits == 16) {
      lo = rysmith::hp::kConcreteInt_I16_Lo;
      hi = rysmith::hp::kConcreteInt_I16_Hi;
    } else if (bits == 32) {
      lo = rysmith::hp::kConcreteInt_I32_Lo;
      hi = rysmith::hp::kConcreteInt_I32_Hi;
    } else if (bits == 64) {
      lo = rysmith::hp::kConcreteInt_I64_Lo;
      hi = rysmith::hp::kConcreteInt_I64_Hi;
    } else if (bits >= 2 && bits < 64) {
      // [P7] Custom iN widths span the full signed range, mirroring the
      // standard widths above. The typechecker's strict literal range
      // check makes anything wider a hard error.
      hi = (int64_t(1) << (bits - 1)) - 1;
      lo = -hi - 1;
    }
    return {lo, hi};
  }

  static int64_t pickConcreteIntLit(std::mt19937 &rng, const TypePtr &targetType) {
    auto [lo, hi] = concreteIntRange(targetType);
    std::uniform_int_distribution<int64_t> d(lo, hi);
    return d(rng);
  }

  // Pick a nonzero concrete literal from the per-width pool. Used as
  // the dividend in off-path `lit / %v` and `lit % %v` atoms — div-by-zero
  // on the runtime variable %v is a separate, pre-existing concern (off-
  // path code is sampled around the solver-chosen execution path).
  static int64_t pickConcreteIntLitNonzero(std::mt19937 &rng, const TypePtr &targetType) {
    int64_t v = pickConcreteIntLit(rng, targetType);
    if (v == 0)
      v = 1;
    return v;
  }

  // Generate a random concrete integer literal in [lo, hi] of the given bitwidth.
  static Atom genConcreteIntAtom(std::mt19937 &rng, const TypePtr &targetType) {
    return coefAtom(intCoef(pickConcreteIntLit(rng, targetType)));
  }

  // Generate a concrete float atom
  static Atom genConcreteFloatAtom(std::mt19937 &rng) {
    std::uniform_int_distribution<std::size_t> d(0, rysmith::hp::kFloatLitPoolSize - 1);
    return coefAtom(floatCoef(rysmith::hp::kFloatLitPool[d(rng)]));
  }

  // Build a SelectVal of targetType: either a same-typed scalar local or a
  // literal/sym Coef. For off-path (sym == nullptr), only locals and literals.
  static SelectVal pickSelectVal(
      std::mt19937 &rng, SymCounter *sym, const VarCatalogue &vars, const TypePtr &targetType,
      const std::optional<std::string> &excludeName = std::nullopt
  ) {
    auto scalars = excluding(vars.scalarsOf(targetType), excludeName);
    // `s in [0, 99]` walks the same percent-slot pattern as the atom
    // dispatch tables: [0, kSelectArm_LocalEnd) → local, then sym,
    // then literal. Default split is uniform thirds.
    std::uniform_int_distribution<int> slot(0, 99);
    int s = slot(rng);
    if (s < rysmith::hp::kSelectArm_LocalEnd && !scalars.empty()) {
      auto *v = pickOne(rng, scalars);
      return SelectVal{LValue{LocalId{v->name, {}}, {}, {}}};
    }
    if (s < rysmith::hp::kSelectArm_SymEnd && sym != nullptr && isIntType(targetType)) {
      // [bugfix] Sym slot must produce a sym whose declared type matches
      // `targetType`. SymCounter::nextValue() hardcoded i32, so a select
      // returning i64 ended up with arms of mismatched widths (i32 sym
      // vs i64 literal/local) and the typechecker rejected the program.
      // Skip the sym slot for FP target types (rysmith does not currently
      // mint FP value-syms in this code path); fall through to the
      // float-literal pool below.
      return SelectVal{symCoef(sym->next(SymKind::Value, targetType))};
    }
    // Literal slot draws from the same per-width / FP pool as bare
    // concrete atoms instead of the hardcoded `1` / `1.0`. Pre-R7 every
    // select fallback arm was exactly `1`, which collapses any
    // `select cond, 1, 1` (both literal arms) to a constant — the
    // compiler removes the select entirely and the cond evaluation with
    // it.
    if (isFpType(targetType)) {
      std::uniform_int_distribution<std::size_t> d(0, rysmith::hp::kFloatLitPoolSize - 1);
      return SelectVal{floatCoef(rysmith::hp::kFloatLitPool[d(rng)])};
    }
    return SelectVal{intCoef(pickConcreteIntLit(rng, targetType))};
  }

  // Generate a SelectAtom of the given target type. Cond compares a scalar
  // local to a literal (defensive: same-type) so the typechecker is happy.
  static Atom genSelectAtom(
      std::mt19937 &rng, SymCounter *sym, const VarCatalogue &vars, const TypePtr &targetType,
      const std::optional<std::string> &excludeName = std::nullopt
  ) {
    Cond cond;
    auto allScalars = excluding(vars.allScalars(), excludeName);
    if (!allScalars.empty()) {
      auto *vL = pickOne(rng, allScalars);
      cond.lhs = simpleExpr(rvalAtom(localLV(vL->name)));
      // RHS literal of the lhs var's type. On-path (sym != null) it stays 0
      // so the path constraint is a simple sign test; off-path [P7] it is
      // drawn from the per-width pool — the threshold value is opaque to
      // the compiler either way, but a fixed 0 needlessly homogenises the
      // never-executed arms.
      if (isFpType(vL->type)) {
        double f = 0.0;
        if (!sym) {
          std::uniform_int_distribution<std::size_t> d(0, rysmith::hp::kFloatLitPoolSize - 1);
          f = rysmith::hp::kFloatLitPool[d(rng)];
        }
        cond.rhs = simpleExpr(coefAtom(floatCoef(f)));
      } else {
        int64_t i = sym ? 0 : pickConcreteIntLit(rng, vL->type);
        cond.rhs = simpleExpr(coefAtom(intCoef(i)));
      }
    } else {
      cond.lhs = simpleExpr(coefAtom(intCoef(0)));
      cond.rhs = simpleExpr(coefAtom(intCoef(0)));
    }
    static const RelOp relops[] = {RelOp::EQ, RelOp::NE, RelOp::LT,
                                   RelOp::LE, RelOp::GT, RelOp::GE};
    std::uniform_int_distribution<int> ro(0, 5);
    cond.op = relops[ro(rng)];

    SelectAtom sa;
    sa.cond = std::make_unique<Cond>(std::move(cond));
    sa.vtrue = pickSelectVal(rng, sym, vars, targetType, excludeName);
    sa.vfalse = pickSelectVal(rng, sym, vars, targetType, excludeName);
    return Atom{std::move(sa), {}};
  }

  // Forward declaration — defined later in this file.
  static std::pair<Expr, std::vector<Instr>> genExprWithRequires(
      std::mt19937 &rng, SymCounter *sym, const VarCatalogue &vars, const TypePtr &targetType,
      bool onPath, const ExprGenConfig &cfg,
      const std::optional<std::string> &excludeName = std::nullopt
  );

  // Generate an intrinsic-call atom for the given integer target type.
  // Adds any safety requires from argument generation to extraRequires.
  // Records the (kind, bitwidth) pair in cfg.usedIntrinsics if non-null.
  // Now that the toolchain supports overloaded intrinsic declarations,
  // the same intrinsic can be instantiated at multiple bitwidths.
  static Atom genIntrinsicCallAtom(
      std::mt19937 &rng, SymCounter *sym, const VarCatalogue &vars, const TypePtr &targetType,
      bool onPath, const ExprGenConfig &cfg, std::vector<Instr> &extraRequires,
      const std::optional<std::string> &excludeName = std::nullopt
  ) {
    // Skip i1-returning intrinsics since i1 is not a commonly generated target type.
    const auto &all = getIntrinsicWhitelist();
    uint32_t targetBits = intBitWidth(targetType);
    std::vector<const WhitelistedIntrinsic *> compatible;
    for (const auto &wi: all) {
      if (wi.returnsI1)
        continue;
      // [P7] @bswap is the one width-restricted whitelisted intrinsic: the
      // semchecker rejects it for widths that are not a multiple of 8.
      if (wi.kind == IntrinsicKind::Bswap && targetBits % 8 != 0)
        continue;
      compatible.push_back(&wi);
    }
    if (compatible.empty())
      return coefAtom(intCoef(0));

    std::uniform_int_distribution<int> pick(0, (int) compatible.size() - 1);
    const auto *wi = compatible[pick(rng)];

    // Build argument expressions
    std::vector<std::shared_ptr<Expr>> args;
    for (int pi = 0; pi < wi->paramCount; pi++) {
      if (onPath && sym) {
        auto [ae, areqs] =
            genExprWithRequires(rng, sym, vars, targetType, onPath, cfg, excludeName);
        for (auto &r: areqs)
          extraRequires.push_back(std::move(r));
        args.push_back(std::make_shared<Expr>(std::move(ae)));
      } else {
        auto ae = genExpr(rng, sym, vars, targetType, onPath, cfg, excludeName);
        args.push_back(std::make_shared<Expr>(std::move(ae)));
      }
    }

    // Record the (kind, bitwidth) use
    if (cfg.usedIntrinsics)
      cfg.usedIntrinsics->insert({wi->kind, intBitWidth(targetType)});

    CallAtom ca;
    ca.callee = GlobalId{std::string(wi->name), {}};
    ca.args = std::move(args);
    return Atom{std::move(ca), {}};
  }

  // Generate a single Atom of the given integer type (on-path, uses sym)
  static Atom genIntAtomOnPath(
      std::mt19937 &rng, SymCounter &sym, const VarCatalogue &vars, const TypePtr &targetType,
      const ExprGenConfig &cfg, std::vector<Instr> &extraRequires,
      const std::optional<std::string> &excludeName = std::nullopt
  ) {
    // Collect scalars of the target type for use as RValues
    auto scalarsOfT = excluding(vars.scalarsOf(targetType), excludeName);
    bool hasRval = !scalarsOfT.empty();

    // Also collect scalars of ANY int type for casts
    auto allScalars = excluding(vars.allScalars(), excludeName);
    std::vector<const VarEntry *> otherIntScalars;
    for (auto *v: allScalars)
      if (isIntType(v->type) && !typeEquals(v->type, targetType))
        otherIntScalars.push_back(v);

    std::uniform_int_distribution<int> slot(0, 99);
    int s = slot(rng);

    auto pickRval = [&]() -> RValue {
      assert(hasRval);
      auto *v = pickOne(rng, scalarsOfT);
      return localLV(v->name);
    };

    if (s < rysmith::hp::kIntOnPath_CoefBareEnd) {
      // Standalone coef sym
      return coefAtom(symCoef(sym.nextCoef(targetType)));
    }
    if (s < rysmith::hp::kIntOnPath_MulEnd && hasRval) {
      // Linear: sym * rval
      return opAtom(AtomOpKind::Mul, symCoef(sym.nextCoef(targetType)), pickRval());
    }
    if (s < rysmith::hp::kIntOnPath_BitwiseEnd && cfg.enableAllOps && hasRval) {
      // Bitwise
      static const AtomOpKind bops[] = {AtomOpKind::And, AtomOpKind::Or, AtomOpKind::Xor};
      std::uniform_int_distribution<int> opPick(0, 2);
      return opAtom(bops[opPick(rng)], symCoef(sym.nextCoef(targetType)), pickRval());
    }
    if (s < rysmith::hp::kIntOnPath_ShiftEnd && cfg.enableAllOps && hasRval) {
      // Shift: index_sym << rval  (coef=index sym, rval=variable being shifted)
      // Per the existing pattern: index sym is the VALUE being shifted, rval is shift amount
      // But shift amount must be i32 — we need an i32 rval for the shift amount
      // Actually in SymIR: OpAtom{Shl, coef, rval} = coef SHL rval
      // coef is the value to shift, rval is the shift amount
      // For proper typing: coef must match targetType, rval (shift amount) must be i32
      // So we need an i32 var as rval for shift amount, and use an index sym as the coef
      // The index sym type must also be targetType for type consistency
      auto i32scalars = excluding(vars.scalarsOf(makeI32()), excludeName);
      if (!i32scalars.empty() || isIntType(targetType)) {
        // Use index sym of targetType as the value being shifted
        // use an i32 var as shift amount if available, else use an int literal
        static const AtomOpKind sops[] = {AtomOpKind::Shl, AtomOpKind::Shr, AtomOpKind::LShr};
        std::uniform_int_distribution<int> opPick(0, 2);
        auto idxSym = sym.nextIndex(); // always i32
        // The shift coef should be the same type as targetType for type correctness
        // but nextIndex always returns i32. For non-i32 targets, we use a coef sym instead
        if (intBitWidth(targetType) == 32) {
          if (!i32scalars.empty()) {
            auto *shiftAmt = pickOne(rng, i32scalars);
            return opAtom(sops[opPick(rng)], symCoef(idxSym), localLV(shiftAmt->name));
          }
        } else {
          // Non-i32 target: use coef sym of targetType << i32 literal for shift amount
          // But OpAtom{Shl, coef, rval}: coef must be targetType, rval must be...
          // In SymIR, shift amount is the rval. For i64 << i32, the rval should be i32.
          // Use a concrete integer rval instead:
          (void) idxSym; // index sym was consumed, drop it
          // Just fall through to coef standalone
          return coefAtom(symCoef(sym.nextCoef(targetType)));
        }
      }
      return coefAtom(symCoef(sym.nextCoef(targetType)));
    }
    if (s < rysmith::hp::kIntOnPath_UnaryNotEnd && hasRval) {
      // Unary NOT
      return unaryAtom(pickRval());
    }
    if (s < rysmith::hp::kIntOnPath_CastEnd && !otherIntScalars.empty()) {
      // CastAtom from another int width
      auto *srcVar = pickOne(rng, otherIntScalars);
      CastAtom ca;
      ca.src = LValue{LocalId{srcVar->name, {}}, {}, {}};
      ca.dstType = targetType;
      return Atom{std::move(ca), {}};
    }
    if (s < rysmith::hp::kIntOnPath_DivModEnd && cfg.enableDiv && hasRval) {
      // Div/Mod: OpAtom{Div/Mod, sym_coef, rval}  = sym / rval
      // We need a require(rval != 0) guard on-path
      std::uniform_int_distribution<int> dm(0, 1);
      AtomOpKind op = dm(rng) ? AtomOpKind::Mod : AtomOpKind::Div;
      auto *rv = pickOne(rng, scalarsOfT);
      std::string symName = sym.nextCoef(targetType);
      // Add require: rval != 0
      RequireInstr req;
      req.cond.lhs = simpleExpr(rvalAtom(localLV(rv->name)));
      req.cond.op = RelOp::NE;
      req.cond.rhs = simpleExpr(coefAtom(intCoef(0)));
      req.message = "div nonzero";
      extraRequires.push_back(Instr{std::move(req)});
      return opAtom(op, symCoef(symName), localLV(rv->name));
    }
    if (s < rysmith::hp::kIntOnPath_LoadEnd) {
      // Load from a ptr T var if any exist
      auto ptrs = excluding(vars.ptrsOf(targetType), excludeName);
      if (!ptrs.empty()) {
        auto *pv = pickOne(rng, ptrs);
        return Atom{LoadAtom{localLV(pv->name), {}}, {}};
      }
    }
    if (s < rysmith::hp::kIntOnPath_SelectEnd && cfg.enableSelect) {
      return genSelectAtom(rng, &sym, vars, targetType, excludeName);
    }
    if (s < rysmith::hp::kIntOnPath_IntrinsicEnd && cfg.enableIntrinsics) {
      return genIntrinsicCallAtom(
          rng, &sym, vars, targetType, true, cfg, extraRequires, excludeName
      );
    }
    // Fallback: standalone sym
    return coefAtom(symCoef(sym.nextCoef(targetType)));
  }

  // [P7] Off-path OpAtom coefficient: a same-type LocalId with probability
  // kPOffPathVarCoef (`%a * %b` — every operator admits a LocalId coef per
  // the grammar, both sides are runtime values the compiler can't fold,
  // and the solver never visits off-path blocks), else a per-width
  // literal. `nonzeroLit` keeps the literal branch's dividend nonzero for
  // div/mod (a 0 dividend folds the whole term).
  static Coef pickOffPathIntCoef(
      std::mt19937 &rng, const VarCatalogue &vars, const TypePtr &targetType,
      const std::optional<std::string> &excludeName, bool nonzeroLit
  ) {
    auto pool = excluding(vars.scalarsOf(targetType), excludeName);
    std::uniform_real_distribution<double> coin(0.0, 1.0);
    if (!pool.empty() && coin(rng) < rysmith::hp::kPOffPathVarCoef)
      return LocalOrSymId{LocalId{pickOne(rng, pool)->name, {}}};
    int64_t v = nonzeroLit ? pickConcreteIntLitNonzero(rng, targetType)
                           : pickConcreteIntLit(rng, targetType);
    return IntLit{v, {}};
  }

  // FP analogue. The literal branch draws from kFloatMulCoefPool (dyadic,
  // 0-free) so the term never folds to a constant.
  static Coef pickOffPathFpCoef(
      std::mt19937 &rng, const VarCatalogue &vars, const TypePtr &targetType,
      const std::optional<std::string> &excludeName
  ) {
    auto pool = excluding(vars.scalarsOf(targetType), excludeName);
    std::uniform_real_distribution<double> coin(0.0, 1.0);
    if (!pool.empty() && coin(rng) < rysmith::hp::kPOffPathVarCoef)
      return LocalOrSymId{LocalId{pickOne(rng, pool)->name, {}}};
    std::uniform_int_distribution<std::size_t> d(0, rysmith::hp::kFloatMulCoefPoolSize - 1);
    return FloatLit{rysmith::hp::kFloatMulCoefPool[d(rng)], {}};
  }

  // Generate a single Atom of the given integer type (off-path; never
  // executed at the solved inputs, so the only constraint is that it
  // typechecks and compiles)
  static Atom genIntAtomOffPath(
      std::mt19937 &rng, const VarCatalogue &vars, const TypePtr &targetType,
      const ExprGenConfig &cfg, const std::optional<std::string> &excludeName = std::nullopt
  ) {
    auto scalarsOfT = excluding(vars.scalarsOf(targetType), excludeName);
    bool hasRval = !scalarsOfT.empty();

    auto allScalars = excluding(vars.allScalars(), excludeName);
    std::vector<const VarEntry *> otherIntScalars;
    for (auto *v: allScalars)
      if (isIntType(v->type) && !typeEquals(v->type, targetType))
        otherIntScalars.push_back(v);

    std::uniform_int_distribution<int> slot(0, 99);
    int s = slot(rng);

    if (s < rysmith::hp::kIntOffPath_ConcreteEnd || !hasRval) {
      return genConcreteIntAtom(rng, targetType);
    }
    if (s < rysmith::hp::kIntOffPath_MulEnd) {
      auto *v = pickOne(rng, scalarsOfT);
      return opAtom(
          AtomOpKind::Mul, pickOffPathIntCoef(rng, vars, targetType, excludeName, false),
          localLV(v->name)
      );
    }
    if (s < rysmith::hp::kIntOffPath_BitwiseEnd && cfg.enableAllOps) {
      auto *v = pickOne(rng, scalarsOfT);
      static const AtomOpKind bops[] = {AtomOpKind::And, AtomOpKind::Or, AtomOpKind::Xor};
      std::uniform_int_distribution<int> op(0, 2);
      return opAtom(
          bops[op(rng)], pickOffPathIntCoef(rng, vars, targetType, excludeName, false),
          localLV(v->name)
      );
    }
    if (s < rysmith::hp::kIntOffPath_ShiftEnd && cfg.enableAllOps) {
      // [P7] All three shift ops at any width. The shift amount is a
      // runtime var and may be negative / oversized at runtime — UB the
      // block never reaches; it only has to typecheck (coef width ==
      // rval width) and compile.
      auto *v = pickOne(rng, scalarsOfT);
      static const AtomOpKind sops[] = {AtomOpKind::Shl, AtomOpKind::Shr, AtomOpKind::LShr};
      std::uniform_int_distribution<int> op(0, 2);
      return opAtom(
          sops[op(rng)], pickOffPathIntCoef(rng, vars, targetType, excludeName, false),
          localLV(v->name)
      );
    }
    if (s < rysmith::hp::kIntOffPath_CastEnd && !otherIntScalars.empty()) {
      auto *v = pickOne(rng, otherIntScalars);
      CastAtom ca;
      ca.src = LValue{LocalId{v->name, {}}, {}, {}};
      ca.dstType = targetType;
      return Atom{std::move(ca), {}};
    }
    if (s < rysmith::hp::kIntOffPath_DivModEnd && cfg.enableDiv && hasRval) {
      // The divisor (the runtime LValue %v) carries a div-by-zero risk
      // this slot has always had — irrelevant, the block never executes.
      auto *v = pickOne(rng, scalarsOfT);
      std::uniform_int_distribution<int> dm(0, 1);
      AtomOpKind op = dm(rng) ? AtomOpKind::Mod : AtomOpKind::Div;
      return opAtom(
          op, pickOffPathIntCoef(rng, vars, targetType, excludeName, true), localLV(v->name)
      );
    }
    if (s < rysmith::hp::kIntOffPath_PlainRvalEnd) {
      auto *v = pickOne(rng, scalarsOfT);
      return rvalAtom(localLV(v->name));
    }
    if (s < rysmith::hp::kIntOffPath_LoadEnd) {
      // Load from a ptr T var if available
      auto ptrs = excluding(vars.ptrsOf(targetType), excludeName);
      if (!ptrs.empty()) {
        auto *pv = pickOne(rng, ptrs);
        return Atom{LoadAtom{localLV(pv->name), {}}, {}};
      }
    }
    if (s < rysmith::hp::kIntOffPath_SelectEnd && cfg.enableSelect) {
      return genSelectAtom(rng, /*sym=*/nullptr, vars, targetType, excludeName);
    }
    if (s < rysmith::hp::kIntOffPath_IntrinsicEnd && cfg.enableIntrinsics) {
      std::vector<Instr> dummyReqs; // off-path args are concrete-only, no requires expected
      return genIntrinsicCallAtom(
          rng, /*sym=*/nullptr, vars, targetType, false, cfg, dummyReqs, excludeName
      );
    }
    return genConcreteIntAtom(rng, targetType);
  }

  // Generate a float atom (on-path: cast from i32 sym or concrete float)
  static Atom genFloatAtomOnPath(
      std::mt19937 &rng, SymCounter &sym, const VarCatalogue &vars, const TypePtr &targetType,
      const ExprGenConfig &cfg, const std::optional<std::string> &excludeName = std::nullopt
  ) {
    auto fpVars = excluding(vars.scalarsOf(targetType), excludeName);
    auto i32scalars = excluding(vars.scalarsOf(makeI32()), excludeName);

    std::uniform_int_distribution<int> slot(0, 99);
    int s = slot(rng);

    if (s < rysmith::hp::kFloatOnPath_CastFromI32SymEnd) {
      // Cast from i32 sym to float (keeps SMT in BV theory)
      auto symName = sym.nextValue();
      CastAtom ca;
      ca.src = SymId{symName, {}};
      ca.dstType = targetType;
      return Atom{std::move(ca), {}};
    }
    if (s < rysmith::hp::kFloatOnPath_MulLitEnd && !fpVars.empty()) {
      // Multiply by concrete float literal
      auto *v = pickOne(rng, fpVars);
      std::uniform_int_distribution<std::size_t> ld(0, rysmith::hp::kFloatMulCoefPoolSize - 1);
      return opAtom(
          AtomOpKind::Mul, floatCoef(rysmith::hp::kFloatMulCoefPool[ld(rng)]), localLV(v->name)
      );
    }
    if (s < rysmith::hp::kFloatOnPath_CastFromVarEnd && !i32scalars.empty()) {
      // CastAtom from i32 var
      auto *v = pickOne(rng, i32scalars);
      CastAtom ca;
      ca.src = LValue{LocalId{v->name, {}}, {}, {}};
      ca.dstType = targetType;
      return Atom{std::move(ca), {}};
    }
    if (s < rysmith::hp::kFloatOnPath_SelectEnd && cfg.enableSelect) {
      return genSelectAtom(rng, &sym, vars, targetType, excludeName);
    }
    // Concrete float literal
    return genConcreteFloatAtom(rng);
  }

  // Generate a float atom (off-path: concrete literals only)
  // Off-path FP atom. Pre-restructure this returned a concrete float
  // literal unconditionally — that left every off-path FP expression
  // 100% trivially constant (no runtime LValue ref) so the trivial-
  // shape post-check could never repair it. Allow ~50% var-ref atoms
  // when a same-typed FP var is available so the post-check has a
  // non-trivial source to draw from.
  static Atom genFloatAtomOffPath(
      std::mt19937 &rng, const VarCatalogue &vars, const TypePtr &targetType,
      const std::optional<std::string> &excludeName = std::nullopt
  ) {
    auto fpVars = excluding(vars.scalarsOf(targetType), excludeName);
    std::uniform_int_distribution<int> slot(0, 99);
    int s = slot(rng);
    if (fpVars.empty())
      return genConcreteFloatAtom(rng);
    if (s < rysmith::hp::kFloatOffPath_ReadEnd) {
      auto *v = pickOne(rng, fpVars);
      return rvalAtom(localLV(v->name));
    }
    // [P7] Floats admit Mul / Div / Mod (fmod) per the typechecker; the
    // div-by-zero / overflow UB these can hit at runtime never fires in an
    // off-path block, and bit-exactness is moot for code that never
    // executes — it only has to compile.
    auto pickVar = [&]() { return localLV(pickOne(rng, fpVars)->name); };
    if (s < rysmith::hp::kFloatOffPath_MulEnd)
      return opAtom(
          AtomOpKind::Mul, pickOffPathFpCoef(rng, vars, targetType, excludeName), pickVar()
      );
    if (s < rysmith::hp::kFloatOffPath_DivEnd)
      return opAtom(
          AtomOpKind::Div, pickOffPathFpCoef(rng, vars, targetType, excludeName), pickVar()
      );
    if (s < rysmith::hp::kFloatOffPath_ModEnd)
      return opAtom(
          AtomOpKind::Mod, pickOffPathFpCoef(rng, vars, targetType, excludeName), pickVar()
      );
    return genConcreteFloatAtom(rng);
  }

  // Generate a ptr-type atom. Collects all candidate atoms uniformly so each
  // option (addr, copy, load-from-ptr-ptr) appears with equal probability.
  static Atom genPtrAtom(
      std::mt19937 &rng, const VarCatalogue &vars, const TypePtr &targetType,
      const std::optional<std::string> &excludeName = std::nullopt
  ) {
    assert(isPtrType(targetType));
    TypePtr ptee = pointeeType(targetType);

    std::vector<Atom> options;

    auto nameExcluded = [&](const std::string &n) { return excludeName && n == *excludeName; };

    // addr of any addressable var whose type equals the pointee
    for (auto *v: vars.allAddressable()) {
      if (nameExcluded(v->name))
        continue;
      if (typeEquals(v->type, ptee)) {
        options.push_back(Atom{AddrAtom{localLV(v->name), {}}, {}});
      }
    }

    // copy from an existing ptr T var (same type)
    for (auto *pv: vars.ptrsOf(ptee)) {
      if (nameExcluded(pv->name))
        continue;
      options.push_back(rvalAtom(localLV(pv->name)));
    }

    // load from a ptr ptr T var to materialise a ptr T value
    for (auto *ppv: vars.ptrsOf(targetType)) {
      if (nameExcluded(ppv->name))
        continue;
      options.push_back(Atom{LoadAtom{localLV(ppv->name), {}}, {}});
    }

    // [v0.2.1] PtrIndexAtom: if ptee is scalar and there exists a
    // ptr [N] ptee var, we can ptrindex it at a safe index.
    if (isScalarType(ptee)) {
      for (const auto &v: vars.vars) {
        if (!isPtrType(v.type))
          continue;
        if (nameExcluded(v.name))
          continue;
        auto innerPtee = pointeeType(v.type);
        if (!innerPtee || !std::holds_alternative<ArrayType>(innerPtee->v))
          continue;
        const auto &at = std::get<ArrayType>(innerPtee->v);
        if (typeEquals(at.elem, ptee)) {
          std::uniform_int_distribution<int64_t> idxd(0, (int64_t) at.size - 1);
          PtrIndexAtom pi;
          pi.rval = localLV(v.name);
          pi.index = Index{IntLit{idxd(rng), {}}};
          options.push_back(Atom{std::move(pi), {}});
        }
      }
    }

    // [v0.2.1] PtrFieldAtom: if there exists a ptr @S var where @S has
    // a field of type ptee, we can ptrfield it.
    for (const auto &v: vars.vars) {
      if (!isPtrType(v.type))
        continue;
      if (nameExcluded(v.name))
        continue;
      auto innerPtee = pointeeType(v.type);
      if (!innerPtee || !std::holds_alternative<StructType>(innerPtee->v))
        continue;
      const auto &st = std::get<StructType>(innerPtee->v);
      for (const auto &sd: vars.structDecls) {
        if (sd.name.name != st.name.name)
          continue;
        for (const auto &f: sd.fields) {
          if (typeEquals(f.type, ptee)) {
            PtrFieldAtom pf;
            pf.rval = localLV(v.name);
            pf.field = f.name;
            options.push_back(Atom{std::move(pf), {}});
          }
        }
      }
    }

    if (!options.empty()) {
      std::uniform_int_distribution<int> d(0, (int) options.size() - 1);
      return std::move(options[d(rng)]);
    }
    return coefAtom(NullLit{{}});
  }

  // ---------------------------------------------------------------------------
  // Trivial-shape post-check helpers
  //
  // Shared by `genExpr` and `genExprWithRequires`. The two callers used to
  // carry near-identical inline lambdas + post-check blocks; factoring
  // here keeps the trivial-shape contract in one place. Only int / fp
  // target types are supported — ptr / vec / struct are shape-restricted
  // by their own dispatch and shouldn't be reshaped here.
  // ---------------------------------------------------------------------------

  // Thin wrapper around the per-direction × per-class atom dispatch.
  // Threads `extraRequires` so on-path int atoms can publish their
  // div-by-zero guards (the off-path/FP paths don't touch it).
  static Atom genOneAtomOfType(
      std::mt19937 &rng, SymCounter *sym, const VarCatalogue &vars, const TypePtr &targetType,
      bool onPath, const ExprGenConfig &cfg, std::vector<Instr> &extraRequires,
      const std::optional<std::string> &excludeName
  ) {
    if (isIntType(targetType))
      return (onPath && sym)
                 ? genIntAtomOnPath(rng, *sym, vars, targetType, cfg, extraRequires, excludeName)
                 : genIntAtomOffPath(rng, vars, targetType, cfg, excludeName);
    return (onPath && sym) ? genFloatAtomOnPath(rng, *sym, vars, targetType, cfg, excludeName)
                           : genFloatAtomOffPath(rng, vars, targetType, excludeName);
  }

  // [P3] Build a cheap, solver-linear atom that reads a runtime LValue and
  // is UB-safe. The chain, each step falling through when its pool is
  // empty:
  //   1. concrete-coef multiply or plain read of a same-type scalar — the
  //      coef is a moderate nonzero literal (kCheapMulCoef*) for ints and a
  //      kFloatMulCoefPool dyadic for floats, so the on-path no-overflow
  //      constraint degrades to a loose linear bound instead of a nonlinear
  //      `sym * var` term (`allowBareRead=false` forces the multiply so the
  //      result can replace a plain copy without recreating one);
  //   2. a cast from a differently-typed scalar, restricted to total
  //      conversions — int→int, int→fp, and the widening f32→f64. A
  //      narrowing fp→fp (f64→f32) can overflow to ±inf and fp→int can
  //      be out-of-range, both UB, so they are excluded;
  //   3. a load through a ptr-to-targetType.
  // Returns nullopt when no readable LValue exists besides the excluded
  // LHS — a genuinely input-free pool cannot carry a runtime dependency.
  static std::optional<Atom> genCheapLinearAtom(
      std::mt19937 &rng, const VarCatalogue &vars, const TypePtr &targetType,
      const std::optional<std::string> &excludeName, bool allowBareRead
  ) {
    auto same = excluding(vars.scalarsOf(targetType), excludeName);
    if (!same.empty()) {
      auto *v = pickOne(rng, same);
      std::uniform_int_distribution<int> slot(0, 99);
      bool mul = !allowBareRead || slot(rng) < rysmith::hp::kCheapAtom_MulEnd;
      if (mul && isIntType(targetType)) {
        std::uniform_int_distribution<int64_t> d(
            rysmith::hp::kCheapMulCoefLo, rysmith::hp::kCheapMulCoefHi
        );
        int64_t c = d(rng);
        if (c == 0)
          c = 2; // a 0 coef folds the term and drops the read
        return opAtom(AtomOpKind::Mul, intCoef(c), localLV(v->name));
      }
      if (mul && isFpType(targetType)) {
        std::uniform_int_distribution<std::size_t> d(0, rysmith::hp::kFloatMulCoefPoolSize - 1);
        return opAtom(
            AtomOpKind::Mul, floatCoef(rysmith::hp::kFloatMulCoefPool[d(rng)]), localLV(v->name)
        );
      }
      return rvalAtom(localLV(v->name));
    }
    auto scalarWidth = [](const TypePtr &t) -> uint32_t {
      if (isIntType(t))
        return intBitWidth(t);
      if (auto *ft = std::get_if<FloatType>(&t->v))
        return ft->kind == FloatType::Kind::F32 ? 32u : 64u;
      return 0;
    };
    auto safeCastSource = [&](const TypePtr &src) -> bool {
      if (typeEquals(src, targetType))
        return false; // same type → covered by the read/mul step
      if (isIntType(targetType))
        return isIntType(src); // int→int is total; fp→int can be range-UB
      if (isIntType(src))
        return true;                                                      // int→fp is always finite
      return isFpType(src) && scalarWidth(src) < scalarWidth(targetType); // widening only
    };
    auto allScalars = excluding(vars.allScalars(), excludeName);
    for (auto *v: allScalars) {
      if (!safeCastSource(v->type))
        continue;
      CastAtom ca;
      ca.src = LValue{LocalId{v->name, {}}, {}, {}};
      ca.dstType = targetType;
      return Atom{std::move(ca), {}};
    }
    auto ptrs = excluding(vars.ptrsOf(targetType), excludeName);
    if (!ptrs.empty()) {
      auto *pv = pickOne(rng, ptrs);
      return Atom{LoadAtom{localLV(pv->name), {}}, {}};
    }
    return std::nullopt;
  }

  // Generate an atom that carries a runtime LValue dependency.
  //
  // On-path the cheap-linear chain is used DIRECTLY: rerolling the slot
  // table would converge on `sym * var` — a fresh free variable plus a
  // nonlinear BV multiply, the most solver-expensive replacement possible
  // for what was a solver-free all-literal RHS (P3).
  //
  // Off-path (solver never looks) the standard dispatch is rerolled up to
  // `kAtomRerollMaxAttempts` times for shape diversity before the same
  // cheap chain takes over. When even the chain has no readable LValue the
  // last trivial roll is returned — the kPAllowAllLiteral slice absorbs it.
  //
  // `allowBareRead=false` additionally rules out a bare RValueAtom result:
  // a caller about to use the atom as a whole single-atom RHS would
  // otherwise manufacture a plain copy, whose rate must stay solely under
  // kPAllowPlainCopy's control (P6).
  static Atom genNonTrivialAtomOfType(
      std::mt19937 &rng, SymCounter *sym, const VarCatalogue &vars, const TypePtr &targetType,
      bool onPath, const ExprGenConfig &cfg, std::vector<Instr> &extraRequires,
      const std::optional<std::string> &excludeName, bool allowBareRead = true
  ) {
    if (onPath && !cfg.condContext) {
      if (auto cheap = genCheapLinearAtom(rng, vars, targetType, excludeName, allowBareRead))
        return std::move(*cheap);
      return isFpType(targetType) ? genConcreteFloatAtom(rng) : genConcreteIntAtom(rng, targetType);
    }
    Atom last =
        genOneAtomOfType(rng, sym, vars, targetType, onPath, cfg, extraRequires, excludeName);
    auto acceptable = [&](const Atom &a) {
      return !isTriviallyConstantAtom(a) &&
             (allowBareRead || !std::holds_alternative<RValueAtom>(a.v));
    };
    if (acceptable(last))
      return last;
    for (int r = 1; r < rysmith::hp::kAtomRerollMaxAttempts; r++) {
      Atom cand =
          genOneAtomOfType(rng, sym, vars, targetType, onPath, cfg, extraRequires, excludeName);
      if (acceptable(cand))
        return cand;
      last = std::move(cand);
    }
    if (auto cheap = genCheapLinearAtom(rng, vars, targetType, excludeName, allowBareRead))
      return std::move(*cheap);
    return last;
  }

  // Apply the trivial-shape post-check in place. With probability
  // `1 - kPAllowAllLiteral`, if every atom is trivially constant the
  // single-atom case grows to two atoms by appending a non-trivial
  // tail; the multi-atom case has its last atom rewritten. Replacing
  // the last (not the first) atom keeps the typechecker's first-atom
  // width inference intact.
  static void rewriteIfAllTriviallyConstant(
      std::mt19937 &rng, SymCounter *sym, const VarCatalogue &vars, const TypePtr &targetType,
      bool onPath, const ExprGenConfig &cfg, std::vector<Instr> &extraRequires,
      const std::optional<std::string> &excludeName, std::vector<Atom> &atoms
  ) {
    if (!isIntType(targetType) && !isFpType(targetType))
      return;
    std::uniform_real_distribution<double> allowCoin(0.0, 1.0);
    if (allowCoin(rng) < rysmith::hp::kPAllowAllLiteral)
      return;
    bool allTrivial = std::all_of(atoms.begin(), atoms.end(), [](const Atom &a) {
      return isTriviallyConstantAtom(a);
    });
    if (!allTrivial)
      return;
    // [P6] When the replacement becomes the WHOLE RHS (single atom replaced
    // in place), a bare read would manufacture a plain copy out of an
    // all-literal RHS — copy frequency must stay solely under
    // kPAllowPlainCopy's control, so bare reads are ruled out here. Tail
    // positions (append / last-atom replacement) keep them: a read joined
    // by +/- is not a copy.
    bool inPlaceWholeRhs = atoms.size() == 1 && cfg.maxAtoms <= 1;
    Atom replacement = genNonTrivialAtomOfType(
        rng, sym, vars, targetType, onPath, cfg, extraRequires, excludeName,
        /*allowBareRead=*/!inPlaceWholeRhs
    );
    if (atoms.size() == 1) {
      if (cfg.maxAtoms > 1) {
        atoms.push_back(std::move(replacement));
      } else {
        atoms[0] = std::move(replacement);
      }
    } else {
      atoms.back() = std::move(replacement);
    }
  }

  // ---------------------------------------------------------------------------
  // Main genExpr
  // ---------------------------------------------------------------------------

  Expr genExpr(
      std::mt19937 &rng, SymCounter *sym, const VarCatalogue &vars, const TypePtr &targetType,
      bool onPath, const ExprGenConfig &cfg, const std::optional<std::string> &excludeName
  ) {
    // Build a 1-3 atom expression (first + rest).
    // Note: div/mod safety requires are lost in this public API;
    // use genBlockStmts which calls genExprWithRequires internally.
    std::vector<Instr> dummyReqs;
    std::vector<Atom> atoms;

    std::uniform_int_distribution<int> nAtomsDist(cfg.minAtoms, cfg.maxAtoms);
    int nAtoms = nAtomsDist(rng);

    // For ptr types, always single atom
    if (isPtrType(targetType)) {
      nAtoms = 1;
    }
    // For float types, limit to 1-2 atoms
    if (isFpType(targetType) && nAtoms > 2) {
      nAtoms = 2;
    }
    // [v0.2.1] Vec types: 1-2 atoms (lane-wise +/- is valid).
    if (isVecType(targetType)) {
      nAtoms = std::min(nAtoms, 2);
    }

    for (int i = 0; i < nAtoms; i++) {
      Atom a;
      if (isIntType(targetType)) {
        if (onPath && sym) {
          a = genIntAtomOnPath(rng, *sym, vars, targetType, cfg, dummyReqs, excludeName);
        } else {
          a = genIntAtomOffPath(rng, vars, targetType, cfg, excludeName);
        }
      } else if (isFpType(targetType)) {
        if (onPath && sym) {
          a = genFloatAtomOnPath(rng, *sym, vars, targetType, cfg, excludeName);
        } else {
          a = genFloatAtomOffPath(rng, vars, targetType, excludeName);
        }
      } else if (isPtrType(targetType)) {
        a = genPtrAtom(rng, vars, targetType, excludeName);
      } else if (isVecType(targetType)) {
        // [v0.2.1] Vec atom generation.
        auto vecs = excluding(vars.vecsOf(targetType), excludeName);
        const auto &vt = std::get<VecType>(targetType->v);

        if (i == 0) {
          // First atom MUST be vec-typed (RValueAtom or OpAtom on a vec var).
          // A bare scalar literal as first atom would fail the typechecker.
          if (!vecs.empty()) {
            std::uniform_int_distribution<int> slot(0, 99);
            int s = slot(rng);
            if (s < rysmith::hp::kVecCopyEnd) {
              auto *v = pickOne(rng, vecs);
              a = rvalAtom(localLV(v->name));
            } else if (s < rysmith::hp::kVecSymMulEnd && onPath && sym) {
              auto *v = pickOne(rng, vecs);
              a = opAtom(AtomOpKind::Mul, symCoef(sym->nextCoef(vt.elem)), localLV(v->name));
            } else if (s < rysmith::hp::kVecConcMulEnd) {
              auto *v = pickOne(rng, vecs);
              int64_t c = std::uniform_int_distribution<
                  int64_t>(rysmith::hp::kVecConcMulLo, rysmith::hp::kVecConcMulHi)(rng);
              if (c == 0)
                c = 1;
              a = opAtom(AtomOpKind::Mul, intCoef(c), localLV(v->name));
            } else {
              auto *v = pickOne(rng, vecs);
              a = rvalAtom(localLV(v->name));
            }
          } else {
            // No vec vars — shouldn't happen for whole-vec assign (guarded
            // by genBlockStmts), but safety fallback.
            CoefAtom ca;
            ca.coef = IntLit{0, {}};
            a = Atom{std::move(ca), {}};
          }
        } else {
          // Tail atom (i > 0): can be vec-typed OR a broadcast scalar literal.
          std::uniform_int_distribution<int> tailSlot(0, 99);
          int ts = tailSlot(rng);
          if (ts < rysmith::hp::kVecTailCopyEnd && !vecs.empty()) {
            // Vec copy (lane-wise +/- with another vec var).
            auto *v = pickOne(rng, vecs);
            a = rvalAtom(localLV(v->name));
          } else if (ts < rysmith::hp::kVecTailOpEnd && !vecs.empty()) {
            // OpAtom on vec var.
            auto *v = pickOne(rng, vecs);
            int64_t c = std::uniform_int_distribution<
                int64_t>(rysmith::hp::kVecConcMulLo, rysmith::hp::kVecConcMulHi)(rng);
            if (c == 0)
              c = 1;
            a = opAtom(AtomOpKind::Mul, intCoef(c), localLV(v->name));
          } else {
            // Broadcast scalar literal (valid in +/- tail position). The
            // int draw uses the same per-width concrete pool as bare
            // literal atoms — pre-consolidation this was a hardcoded
            // [-8, 8] uniform, which narrowed the literal magnitude
            // distribution well below what the compiler folds.
            CoefAtom ca;
            if (isFpType(vt.elem)) {
              std::uniform_int_distribution<std::size_t> fd(0, rysmith::hp::kFloatLitPoolSize - 1);
              ca.coef = FloatLit{rysmith::hp::kFloatLitPool[fd(rng)], {}};
            } else {
              ca.coef = IntLit{pickConcreteIntLit(rng, vt.elem), {}};
            }
            a = Atom{std::move(ca), {}};
          }
        }
      } else {
        // Unknown type (e.g. struct) — fallback to i32 concrete atom
        a = genConcreteIntAtom(rng, makeI32());
      }
      atoms.push_back(std::move(a));
    }

    // Trivial-shape post-check. `genExpr` is also called for intrinsic
    // arguments — `call @foo(3, 5)` is just as foldable as a body
    // `%x = 3 + 5;` and benefits from the same reshape.
    rewriteIfAllTriviallyConstant(
        rng, sym, vars, targetType, onPath, cfg, dummyReqs, excludeName, atoms
    );

    // Build Expr from atoms
    Expr expr;
    expr.first = std::move(atoms[0]);
    std::uniform_real_distribution<double> addOpCoin(0.0, 1.0);
    for (std::size_t i = 1; i < atoms.size(); i++) {
      // For float, always use Plus (subtraction is fine but let's keep it simple)
      AddOp op =
          isFpType(targetType)
              ? AddOp::Plus
              : (addOpCoin(rng) < rysmith::hp::kPTailAddOpIsPlus ? AddOp::Plus : AddOp::Minus);
      expr.rest.push_back({op, std::move(atoms[i]), {}});
    }

    (void) dummyReqs; // div/mod requires are handled by genExprWithRequires / genBlockStmts
    return expr;
  }

  // ---------------------------------------------------------------------------
  // genExprWithRequires — internal, returns both expr and any safety requires
  // ---------------------------------------------------------------------------

  // Generate the first atom of an off-path integer expression.
  // MUST produce an atom whose type is unambiguously targetType (not just
  // inferred from context). This is because the TypeChecker uses the first
  // atom's type to set the expected type for tail atoms. If the first atom
  // is a bare IntLit with no context, it defaults to i32 which may mismatch
  // the tail atoms that reference actual vars of targetType.
  static Atom genFirstIntAtomOffPath(
      std::mt19937 &rng, const VarCatalogue &vars, const TypePtr &targetType,
      [[maybe_unused]] const ExprGenConfig &cfg,
      const std::optional<std::string> &excludeName = std::nullopt
  ) {
    auto scalarsOfT = excluding(vars.scalarsOf(targetType), excludeName);
    auto allScalars = excluding(vars.allScalars(), excludeName);
    std::vector<const VarEntry *> otherIntScalars;
    for (auto *v: allScalars)
      if (isIntType(v->type) && !typeEquals(v->type, targetType))
        otherIntScalars.push_back(v);

    if (!scalarsOfT.empty()) {
      // RValueAtom: type is determined by the variable's declared type
      auto *v = pickOne(rng, scalarsOfT);
      return rvalAtom(localLV(v->name));
    }
    if (!otherIntScalars.empty()) {
      // CastAtom: explicitly typed by dstType
      auto *v = pickOne(rng, otherIntScalars);
      CastAtom ca;
      ca.src = LValue{LocalId{v->name, {}}, {}, {}};
      ca.dstType = targetType;
      return Atom{std::move(ca), {}};
    }
    // No variables at all — bare literal (OK for single-atom expressions
    // where expectedBits comes from the assignment context)
    return genConcreteIntAtom(rng, targetType);
  }

  // Generate the first atom of an on-path integer expression.
  // For non-i32 types, must produce an atom whose type is unambiguously
  // targetType so the printed concrete program (after symbol substitution)
  // still type-checks. A bare CoefAtom{SymId} prints as a bare integer after
  // substitution, which the TypeChecker defaults to i32 — causing a bitwidth
  // mismatch when targetType is wider/narrower. We use CastAtom{SymId, dst}
  // which prints as "<value> as <type>" and preserves the type annotation.
  static Atom genFirstIntAtomOnPath(
      std::mt19937 &rng, SymCounter &sym, const VarCatalogue &vars, const TypePtr &targetType,
      const ExprGenConfig &cfg, std::vector<Instr> &extraRequires,
      const std::optional<std::string> &excludeName = std::nullopt
  ) {
    // For i32, a bare CoefAtom is fine (i32 is the default inferred type).
    if (intBitWidth(targetType) == 32) {
      return genIntAtomOnPath(rng, sym, vars, targetType, cfg, extraRequires, excludeName);
    }
    // Non-i32: prefer RValueAtom (variable of targetType) since its type is
    // inferred from the declaration. Failing that, wrap a sym coef in a CastAtom.
    auto scalarsOfT = excluding(vars.scalarsOf(targetType), excludeName);
    if (!scalarsOfT.empty()) {
      // 50% chance of RValueAtom vs CastAtom{sym}
      std::uniform_int_distribution<int> coin(0, 1);
      if (coin(rng)) {
        auto *v = pickOne(rng, scalarsOfT);
        return rvalAtom(localLV(v->name));
      }
    }
    // CastAtom{SymId, targetType}: prints as "<solved_value> as <type>"
    CastAtom ca;
    ca.src = SymId{sym.nextCoef(targetType), {}};
    ca.dstType = targetType;
    return Atom{std::move(ca), {}};
  }

  static std::pair<Expr, std::vector<Instr>> genExprWithRequires(
      std::mt19937 &rng, SymCounter *sym, const VarCatalogue &vars, const TypePtr &targetType,
      bool onPath, const ExprGenConfig &cfg, const std::optional<std::string> &excludeName
  ) {
    std::vector<Instr> reqs;
    std::vector<Atom> atoms;

    std::uniform_int_distribution<int> nAtomsDist(cfg.minAtoms, cfg.maxAtoms);
    int nAtoms = nAtomsDist(rng);

    if (isPtrType(targetType))
      nAtoms = 1;
    if (isFpType(targetType) && nAtoms > 2)
      nAtoms = 2;

    for (int i = 0; i < nAtoms; i++) {
      Atom a;
      if (isIntType(targetType)) {
        if (onPath && sym) {
          // For i==0, use genFirstIntAtomOnPath to ensure non-i32 types get an
          // explicitly-typed first atom. A bare CoefAtom{SymId} would print as a
          // bare integer literal after model substitution, defaulting to i32.
          if (i == 0) {
            a = genFirstIntAtomOnPath(rng, *sym, vars, targetType, cfg, reqs, excludeName);
          } else {
            a = genIntAtomOnPath(rng, *sym, vars, targetType, cfg, reqs, excludeName);
          }
        } else {
          // The first atom of an off-path expression must be explicitly typed.
          // The TypeChecker evaluates the condition LHS with expectedBits=nullopt,
          // meaning a bare IntLit first atom defaults to i32 regardless of condType.
          // Using genFirstIntAtomOffPath for i==0 ensures the first atom is always
          // a RValueAtom or CastAtom whose type is unambiguous.
          if (i == 0) {
            a = genFirstIntAtomOffPath(rng, vars, targetType, cfg, excludeName);
          } else {
            a = genIntAtomOffPath(rng, vars, targetType, cfg, excludeName);
          }
        }
      } else if (isFpType(targetType)) {
        if (onPath && sym) {
          a = genFloatAtomOnPath(rng, *sym, vars, targetType, cfg, excludeName);
        } else {
          a = genFloatAtomOffPath(rng, vars, targetType, excludeName);
        }
      } else if (isPtrType(targetType)) {
        a = genPtrAtom(rng, vars, targetType, excludeName);
      } else if (isVecType(targetType)) {
        // [v0.2.1] Vec target in genExprWithRequires — delegate to the
        // same logic as genExpr's vec branch.
        auto vecs = excluding(vars.vecsOf(targetType), excludeName);
        const auto &vt = std::get<VecType>(targetType->v);
        if (i == 0 && !vecs.empty()) {
          auto *v = pickOne(rng, vecs);
          if (onPath && sym) {
            std::uniform_int_distribution<int> slot(0, 1);
            if (slot(rng) == 0)
              a = rvalAtom(localLV(v->name));
            else
              a = opAtom(AtomOpKind::Mul, symCoef(sym->nextCoef(vt.elem)), localLV(v->name));
          } else {
            a = rvalAtom(localLV(v->name));
          }
        } else if (i > 0 && !vecs.empty()) {
          std::uniform_int_distribution<int> ts(0, 1);
          if (ts(rng) == 0) {
            auto *v = pickOne(rng, vecs);
            a = rvalAtom(localLV(v->name));
          } else {
            CoefAtom ca;
            if (isFpType(vt.elem))
              ca.coef = FloatLit{1.0, {}};
            else
              ca.coef = IntLit{1, {}};
            a = Atom{std::move(ca), {}};
          }
        } else {
          CoefAtom ca;
          ca.coef = IntLit{0, {}};
          a = Atom{std::move(ca), {}};
        }
      } else {
        // Struct type — generate i32 concrete (struct assignments handled specially)
        a = genConcreteIntAtom(rng, makeI32());
      }
      atoms.push_back(std::move(a));
    }

    // R3 + P6: a single-atom bare-RValueAtom RHS (`%v = %w;`) is reshaped
    // so it can't bulk-collapse under SCCP — except for a kPAllowPlainCopy
    // slice that survives verbatim: copy propagation is a distinct dataflow
    // shape the optimizer exercises, and forbidding it outright homogenises
    // every def into an arithmetic join. Self-assigns remain impossible
    // (excludeName filters the LHS from every pool). Skip for ptr/vec:
    // ptr is forced single-atom with `excludeName` already filtering
    // the pool; vec already mixes whole-vec copy with lane-wise tails
    // in its own dispatch.
    std::uniform_real_distribution<double> copyCoin(0.0, 1.0);
    if (atoms.size() == 1 && std::holds_alternative<RValueAtom>(atoms[0].v) &&
        (isIntType(targetType) || isFpType(targetType)) &&
        copyCoin(rng) >= rysmith::hp::kPAllowPlainCopy) {
      if (cfg.maxAtoms > 1) {
        // [P3] On-path the appended tail is a cheap linear atom — a slot-
        // table roll would mostly mint a fresh sym (free variable) or a
        // nonlinear `sym * var` for what is purely an anti-copy reshape.
        std::optional<Atom> tail =
            (onPath && !cfg.condContext)
                ? genCheapLinearAtom(rng, vars, targetType, excludeName, true)
                : std::nullopt;
        if (tail) {
          atoms.push_back(std::move(*tail));
        } else {
          atoms.push_back(
              genOneAtomOfType(rng, sym, vars, targetType, onPath, cfg, reqs, excludeName)
          );
        }
      } else {
        // [P3] In-place replacement must not be another bare read (that
        // would recreate a plain copy of a different var), so the cheap
        // chain is asked for a multiply / cast / load.
        std::optional<Atom> replacement =
            (onPath && !cfg.condContext)
                ? genCheapLinearAtom(rng, vars, targetType, excludeName, false)
                : std::nullopt;
        for (int attempt = 0; !replacement && attempt < 10; attempt++) {
          Atom cand = genOneAtomOfType(rng, sym, vars, targetType, onPath, cfg, reqs, excludeName);
          if (!std::holds_alternative<RValueAtom>(cand.v))
            replacement = std::move(cand);
        }
        if (replacement) {
          atoms[0] = std::move(*replacement);
        } else {
          CoefAtom ca;
          if (onPath && sym) {
            ca.coef = SymId{sym->nextCoef(targetType), {}};
          } else {
            if (isFpType(targetType)) {
              ca.coef = FloatLit{1.0, {}};
            } else {
              ca.coef = IntLit{1, {}};
            }
          }
          atoms[0] = Atom{std::move(ca), {}};
        }
      }
    }

    rewriteIfAllTriviallyConstant(
        rng, sym, vars, targetType, onPath, cfg, reqs, excludeName, atoms
    );

    Expr expr;
    expr.first = std::move(atoms[0]);
    std::uniform_real_distribution<double> addOpCoin(0.0, 1.0);
    for (std::size_t i = 1; i < atoms.size(); i++) {
      AddOp op =
          isFpType(targetType)
              ? AddOp::Plus
              : (addOpCoin(rng) < rysmith::hp::kPTailAddOpIsPlus ? AddOp::Plus : AddOp::Minus);
      expr.rest.push_back({op, std::move(atoms[i]), {}});
    }

    return {std::move(expr), std::move(reqs)};
  }

  // ---------------------------------------------------------------------------
  // genCond
  // ---------------------------------------------------------------------------

  Cond genCond(
      std::mt19937 &rng, SymCounter *sym, const VarCatalogue &vars, bool onPath,
      const ExprGenConfig &cfg
  ) {
    // Pick a random integer scalar type from available vars for the condition
    auto scalars = vars.allScalars();
    TypePtr condType = makeI32(); // default
    if (!scalars.empty()) {
      // Filter to int types only (can't compare floats with relational ops in SymIR)
      std::vector<const VarEntry *> intScalars;
      for (auto *v: scalars)
        if (isIntType(v->type))
          intScalars.push_back(v);
      if (!intScalars.empty()) {
        auto *v = pickOne(rng, intScalars);
        condType = v->type;
      }
    }

    // [P3] Mark the arm expressions as condition context so their trivial-
    // shape replacements keep minting syms — conditions are the solver's
    // handles for driving the path; sym-free arms starve it of freedom and
    // inflate the UNSAT/retry rate.
    ExprGenConfig condCfg = cfg;
    condCfg.condContext = true;

    std::vector<Instr> lhsReqs, rhsReqs;
    auto [lhs, lreqs] = genExprWithRequires(rng, sym, vars, condType, onPath, condCfg);
    auto [rhs, rreqs] = genExprWithRequires(rng, sym, vars, condType, onPath, condCfg);
    // Note: condition requires are discarded here (condition can't have inline requires easily)
    // This is acceptable since the requires from div are an optional safety feature
    (void) lreqs;
    (void) rreqs;

    static const RelOp relOps[] = {RelOp::LT, RelOp::GT, RelOp::LE,
                                   RelOp::GE, RelOp::EQ, RelOp::NE};
    std::uniform_int_distribution<int> opPick(0, 5);

    return Cond{std::move(lhs), relOps[opPick(rng)], std::move(rhs), {}};
  }

  // ---------------------------------------------------------------------------
  // genBlockStmts
  // ---------------------------------------------------------------------------

  std::vector<Instr> genBlockStmts(
      std::mt19937 &rng, SymCounter *sym, const VarCatalogue &vars, int nStmts, bool onPath,
      const ExprGenConfig &cfg
  ) {
    std::vector<Instr> result;
    std::uniform_real_distribution<double> prob(0.0, 1.0);

    // Collect mutable (non-ptr) vars for assignment targets
    // We assign to scalars and array/struct elements.
    // [v0.2.2] Function parameters are immutable per spec §3.5.2 — exclude.
    std::vector<const VarEntry *> allVars;
    for (const auto &v: vars.vars)
      if (!v.isParam)
        allVars.push_back(&v);

    // Collect ptr vars for store operations
    std::vector<const VarEntry *> ptrVars;
    for (const auto &v: vars.vars)
      if (isPtrType(v.type))
        ptrVars.push_back(&v);

    // Splice a single StoreInstr (plus its safety requires) into `result`.
    // Returns false when no usable ptr exists or the chosen pointee type
    // is one we don't store through (aggregate pointees aren't supported
    // by the SymIR `store` form). The Bernoulli chain below stops rolling
    // on the first false to avoid burning the rng on no-ops.
    auto tryEmitStore = [&]() -> bool {
      if (ptrVars.empty())
        return false;
      auto *pv = pickOne(rng, ptrVars);
      TypePtr ptee = pointeeType(pv->type);
      if (!isIntType(ptee) && !isFpType(ptee))
        return false;
      Expr ptrExpr = simpleExpr(rvalAtom(localLV(pv->name)));
      auto [valExpr, reqs] = genExprWithRequires(rng, sym, vars, ptee, onPath, cfg);
      if (onPath) {
        for (auto &req: reqs)
          result.push_back(std::move(req));
      }
      StoreInstr st;
      st.ptr = std::move(ptrExpr);
      st.val = std::move(valExpr);
      result.push_back(Instr{std::move(st)});
      return true;
    };

    for (int s = 0; s < nStmts; s++) {
      // Bernoulli chain of StoreInstrs spliced before this AssignInstr.
      // Each successful roll emits one store and rolls again; the chain
      // stops on the first failed roll. Stores do NOT consume the
      // `nStmts` budget — the budget tracks AssignInstrs only, so the
      // name (`n-stmts`) matches what it counts. With the default
      // `kPStoreBeforeAssign = 0.25` the expected chain length is
      // p / (1 - p) ≈ 0.33 stores per assignment. Pre-restructure a
      // single coin toss decided "this slot is a store XOR an assign",
      // which meant a heavy store density starved the assign count
      // and `--n-stmts` no longer matched the body's assignment
      // population.
      while (prob(rng) < rysmith::hp::kPStoreBeforeAssign) {
        if (!tryEmitStore())
          break;
      }

      // Try up to `kAssignTargetMaxAttempts` LHS picks before giving up.
      // Aggregate LHSs occasionally land on a slot that can't be built
      // (array-of-nested-array, struct with only ptr fields, etc.); each
      // such pick is a `continue` to the next attempt rather than a
      // `continue` to the next `nStmts` slot, so `--n-stmts N` actually
      // produces N AssignInstrs per block.
      bool assignEmitted = false;
      for (int attempt = 0; attempt < rysmith::hp::kAssignTargetMaxAttempts && !assignEmitted;
           attempt++) {
        if (allVars.empty())
          break;
        auto *lhsVar = pickOne(rng, allVars);

        // Build LHS based on var type
        LValue lhs;
        TypePtr assignType;

        if (isScalarType(lhsVar->type)) {
          lhs = localLV(lhsVar->name);
          assignType = lhsVar->type;
        } else if (std::holds_alternative<ArrayType>(lhsVar->type->v)) {
          const auto &at = std::get<ArrayType>(lhsVar->type->v);
          std::uniform_int_distribution<int64_t> idxd(0, (int64_t) at.size - 1);
          int64_t idx = idxd(rng);
          if (isScalarType(at.elem)) {
            lhs = arrayLV(lhsVar->name, idx);
            assignType = at.elem;
          } else if (std::holds_alternative<StructType>(at.elem->v)) {
            // Array-of-struct: pick a scalar field, generate %a[i].f = expr
            const std::string &ename = std::get<StructType>(at.elem->v).name.name;
            const StructDecl *sd = nullptr;
            for (const auto &decl: vars.structDecls)
              if (decl.name.name == ename) {
                sd = &decl;
                break;
              }
            if (!sd || sd->fields.empty())
              continue;
            std::vector<const FieldDecl *> scalarFields;
            for (const auto &f: sd->fields)
              if (isScalarType(f.type))
                scalarFields.push_back(&f);
            if (scalarFields.empty())
              continue;
            const FieldDecl *f = pickOne(rng, scalarFields);
            lhs = arrayLV(lhsVar->name, idx);
            lhs.accesses.push_back(AccessField{f->name, {}});
            assignType = f->type;
          } else {
            continue; // nested array or other, skip
          }
        } else if (std::holds_alternative<StructType>(lhsVar->type->v)) {
          const std::string &sname = lhsVar->structTypeName;
          const StructDecl *sd = nullptr;
          for (const auto &decl: vars.structDecls)
            if (decl.name.name == sname) {
              sd = &decl;
              break;
            }
          if (!sd || sd->fields.empty()) {
            // Fallback: scalar assignment
            auto scalars = vars.allScalars();
            if (scalars.empty())
              continue;
            auto *sv = pickOne(rng, scalars);
            lhs = localLV(sv->name);
            assignType = sv->type;
          } else {
            std::uniform_int_distribution<int> fpick(0, (int) sd->fields.size() - 1);
            const auto &f = sd->fields[fpick(rng)];
            if (isScalarType(f.type)) {
              lhs = structLV(lhsVar->name, f.name);
              assignType = f.type;
            } else if (std::holds_alternative<ArrayType>(f.type->v)) {
              // Struct-of-array field: pick an element, generate %t.f[i] = expr
              const auto &fat = std::get<ArrayType>(f.type->v);
              if (!isScalarType(fat.elem))
                continue;
              std::uniform_int_distribution<int64_t> idxd2(0, (int64_t) fat.size - 1);
              lhs = structLV(lhsVar->name, f.name);
              lhs.accesses.push_back(AccessIndex{Index{IntLit{idxd2(rng), {}}}, {}});
              assignType = fat.elem;
            } else {
              continue; // ptr field or other, skip
            }
          }
        } else if (isPtrType(lhsVar->type)) {
          // Ptr reassignment: redirect to another target
          lhs = localLV(lhsVar->name);
          // Exclude the LHS ptr from its own RHS pool — `%p = %p;` is
          // a no-op that SCCP folds; the ptr-copy slot of genPtrAtom picks
          // from `vars.ptrsOf(ptee)` which would otherwise include %p.
          // Bare `%p = %q;` (q != p) is intentionally permitted — adding
          // pointer arithmetic like `%p = %q + 1;` would push %p past
          // single-element objects (every reify ptr is `addr %scalar`),
          // making any later `load %p` UB. Aliasing through a plain ptr
          // copy is a useful, semantically distinct shape from a true
          // self-assign.
          Expr rhs = simpleExpr(genPtrAtom(rng, vars, lhsVar->type, lhsVar->name));
          result.push_back(Instr{AssignInstr{std::move(lhs), std::move(rhs), {}}});
          assignEmitted = true;
          continue;
        } else if (isVecType(lhsVar->type)) {
          // [v0.2.1] Vec assignment: whole-vec only if we have another vec
          // var of the same type to copy from (otherwise the typechecker
          // rejects scalar RHS). Fall back to lane write.
          // Exclude LHS from the same-type pool — `%vec = %vec;` is a
          // no-op and the whole-vec RHS expr would otherwise have to pick
          // the same var. With only the LHS available, fall through to
          // lane write so the RHS lives in scalar-pool territory.
          const auto &vt = std::get<VecType>(lhsVar->type->v);
          auto sameVecs = excluding(vars.vecsOf(lhsVar->type), lhsVar->name);
          bool canWholeVec = !sameVecs.empty();
          std::uniform_int_distribution<int> vslot(0, 99);
          if (canWholeVec && vslot(rng) >= rysmith::hp::kVecLaneWriteProb) {
            // Whole-vec assign (copy or broadcast-mul)
            lhs = localLV(lhsVar->name);
            assignType = lhsVar->type;
          } else {
            // Lane write: %vec[i] = scalar_expr
            std::uniform_int_distribution<int64_t> ld(0, (int64_t) vt.size - 1);
            lhs = localLV(lhsVar->name);
            lhs.accesses.push_back(AccessIndex{Index{IntLit{ld(rng), {}}}, {}});
            assignType = vt.elem;
          }
        } else {
          continue; // unknown type, skip
        }

        if (!assignType)
          continue;

        // The LHS root name (e.g. `%v0`, `%a` in `%a[i].f`, `%vec` in
        // `%vec` whole-vec or `%vec[i]` lane write) is forbidden as an
        // RValue anywhere in the RHS. For aggregate LHS (`%a[i]`, `%t.f`),
        // excluding the root has no practical effect (the root is not a
        // scalar and scalar pickers never see it), but it costs nothing.
        auto [rhs, reqs] =
            genExprWithRequires(rng, sym, vars, assignType, onPath, cfg, lhsVar->name);

        // Insert safety requires before assignment (on-path only — off-path
        // blocks are never executed at the solved inputs).
        if (onPath) {
          for (auto &req: reqs)
            result.push_back(std::move(req));
        }

        result.push_back(Instr{AssignInstr{std::move(lhs), std::move(rhs), {}}});
        assignEmitted = true;
      } // end of LHS-pick retry loop
    }

    return result;
  }

} // namespace symir::reify
