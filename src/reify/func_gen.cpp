#include "reify/func_gen.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <functional>
#include <set>
#include <unordered_set>
#include "analysis/intrinsics.hpp"
#include "reify/hyperparameters.hpp"
#include "reify/intrinsic_whitelist.hpp"

namespace refractir::reify {

  // ---------------------------------------------------------------------------
  // Internal helpers
  // ---------------------------------------------------------------------------

  static TypePtr makeI32() {
    return std::make_shared<Type>(Type{IntType{IntType::Kind::I32, {}, {}}, {}});
  }

  static LValue localLV(const std::string &name) { return LValue{LocalId{name, {}}, {}, {}}; }

  static Atom coefAtom(Coef c) { return Atom{CoefAtom{std::move(c), {}}, {}}; }

  static Atom rvalAtom(RValue rv) { return Atom{RValueAtom{std::move(rv), {}}, {}}; }

  static Expr simpleExpr(Atom a) { return Expr{std::move(a), {}, {}}; }

  // ---------------------------------------------------------------------------
  // let-init value sampling
  // ---------------------------------------------------------------------------

  // Signed [min, max] of an N-bit integer type (N ∈ [2, 64]).
  static std::pair<int64_t, int64_t> signedIntRange(uint32_t bits) {
    if (bits >= 64)
      return {INT64_MIN, INT64_MAX};
    int64_t hi = (int64_t(1) << (bits - 1)) - 1;
    return {-hi - 1, hi};
  }

  // Weighted-mixture integer init literal — see hp::kInitInt_* for the
  // three-bucket rationale (small / mid / boundary).
  static int64_t pickInitIntLit(std::mt19937 &rng, const TypePtr &t) {
    auto [lo, hi] = signedIntRange(intBitWidth(t));
    std::uniform_real_distribution<double> coin(0.0, 1.0);
    double u = coin(rng);
    if (u < rysmith::hp::kInitInt_pSmall) {
      int64_t mag = std::min<int64_t>(rysmith::hp::kInitInt_SmallMag, hi);
      std::uniform_int_distribution<int64_t> d(std::max<int64_t>(-mag, lo), mag);
      return d(rng);
    }
    if (u < rysmith::hp::kInitInt_pSmall + rysmith::hp::kInitInt_pMid) {
      std::uniform_int_distribution<int64_t> d(lo, hi);
      return d(rng);
    }
    const int64_t cand[] = {0, -1, 1, lo, hi, lo + 1, hi - 1};
    std::uniform_int_distribution<std::size_t> d(0, sizeof(cand) / sizeof(cand[0]) - 1);
    return cand[d(rng)];
  }

  // Float init literal: a dyadic, bit-exact, f32-safe value from the shared
  // pool (uniform). Same pool the expression generator uses, so the FP
  // serialization / differential-testing invariant is preserved.
  static double pickInitFloatLit(std::mt19937 &rng) {
    std::uniform_int_distribution<std::size_t> d(0, rysmith::hp::kFloatLitPoolSize - 1);
    return rysmith::hp::kFloatLitPool[d(rng)];
  }

  // ---------------------------------------------------------------------------
  // Unified recursive initializer
  // ---------------------------------------------------------------------------

  static InitVal
  makeInitVal(std::mt19937 &rng, const TypePtr &t, const std::vector<StructDecl> &structDecls) {
    if (isIntType(t)) {
      InitVal iv;
      iv.kind = InitVal::Kind::Int;
      iv.value = IntLit{pickInitIntLit(rng, t), {}};
      return iv;
    }
    if (isFpType(t)) {
      InitVal iv;
      iv.kind = InitVal::Kind::Float;
      iv.value = FloatLit{pickInitFloatLit(rng), {}};
      return iv;
    }
    if (isPtrType(t)) {
      InitVal iv;
      iv.kind = InitVal::Kind::Undef;
      return iv;
    }
    if (std::holds_alternative<ArrayType>(t->v)) {
      const auto &at = std::get<ArrayType>(t->v);
      std::vector<InitValPtr> children;
      for (uint64_t i = 0; i < at.size; i++) {
        children.push_back(std::make_shared<InitVal>(makeInitVal(rng, at.elem, structDecls)));
      }
      InitVal iv;
      iv.kind = InitVal::Kind::Aggregate;
      iv.value = std::move(children);
      return iv;
    }
    if (std::holds_alternative<StructType>(t->v)) {
      const std::string &sname = std::get<StructType>(t->v).name.name;
      const StructDecl *sd = nullptr;
      for (const auto &decl: structDecls)
        if (decl.name.name == sname) {
          sd = &decl;
          break;
        }
      if (sd) {
        std::vector<InitValPtr> children;
        for (const auto &field: sd->fields) {
          children.push_back(std::make_shared<InitVal>(makeInitVal(rng, field.type, structDecls)));
        }
        InitVal iv;
        iv.kind = InitVal::Kind::Aggregate;
        iv.value = std::move(children);
        return iv;
      }
    }
    // [v0.2.1] VecType: brace-init with N scalar elements.
    if (std::holds_alternative<VecType>(t->v)) {
      const auto &vt = std::get<VecType>(t->v);
      std::vector<InitValPtr> children;
      for (uint64_t i = 0; i < vt.size; i++) {
        children.push_back(std::make_shared<InitVal>(makeInitVal(rng, vt.elem, structDecls)));
      }
      InitVal iv;
      iv.kind = InitVal::Kind::Aggregate;
      iv.value = std::move(children);
      return iv;
    }
    InitVal iv;
    iv.kind = InitVal::Kind::Undef;
    return iv;
  }

  // ---------------------------------------------------------------------------
  // Checksum builder
  // ---------------------------------------------------------------------------

  // Build the *sum-form* checksum the solver sees: `%_chk = 0;` then
  // one `%_chk = %_chk + atom` per scalar leaf of every let-init local
  // and every parameter. Pointer leaves are deliberately NOT
  // dereferenced here — the underlying object is already hashed via
  // direct access (every pointer in the generated program points to an
  // existing local, set up in the entry block by addr-init), so a
  // `load %p` in the sum would only force the solver to encode a
  // load-dispatch chain for no coverage gain. The CRC32 rewriter
  // (`rewriteExitToCrc32Checksum`) re-adds `load %p` calls post-solve,
  // where the load serves as runtime opacity against the C optimizer
  // rather than an SMT obligation.
  static std::vector<Instr> buildSumChecksum(const VarCatalogue &vars) {
    std::vector<Instr> instrs;
    auto i32 = makeI32();

    // %_chk = 0;
    {
      Expr zero = simpleExpr(coefAtom(IntLit{0, {}}));
      instrs.push_back(Instr{AssignInstr{localLV("%_chk"), std::move(zero), {}}});
    }

    // Emit `%_chk = cast_atom + %_chk` (cast first) or `%_chk = %_chk + rval`.
    // CastAtom must be the FIRST atom — it cannot appear in tail position.
    auto emitChkAccum = [&](Atom valueAtom, bool isCast) {
      Expr rhs;
      if (isCast) {
        rhs.first = std::move(valueAtom);
        rhs.rest.push_back({AddOp::Plus, rvalAtom(localLV("%_chk")), {}});
      } else {
        rhs.first = rvalAtom(localLV("%_chk"));
        rhs.rest.push_back({AddOp::Plus, std::move(valueAtom), {}});
      }
      instrs.push_back(Instr{AssignInstr{localLV("%_chk"), std::move(rhs), {}}});
    };

    // Emit a scalar LValue into the checksum, casting to i32 when needed.
    auto emitScalarLV = [&](LValue lv, const TypePtr &t) {
      if (isIntType(t) && intBitWidth(t) == 32) {
        emitChkAccum(rvalAtom(std::move(lv)), /*isCast=*/false);
      } else if (isScalarType(t)) {
        CastAtom ca;
        ca.src = std::move(lv);
        ca.dstType = i32;
        emitChkAccum(Atom{std::move(ca), {}}, /*isCast=*/true);
      }
    };

    // Look up a struct decl by name; returns nullptr if absent.
    auto findStruct = [&](const std::string &sname) -> const StructDecl * {
      for (const auto &d: vars.structDecls) {
        if (d.name.name == sname)
          return &d;
      }
      return nullptr;
    };

    // Recursively walk every NON-POINTER scalar leaf of `(lv, t)` and
    // fold each into `%_chk`. Aggregate types (Array, Vec, Struct) —
    // and arbitrary nestings thereof — are unrolled at compile time.
    // Pointer leaves are skipped: the rewriter appends `load %p` steps
    // post-solve so the sum stays cheap for the SMT solver while the
    // final CRC32 still folds every byte the pointer chain can reach.
    std::function<void(LValue, const TypePtr &)> emitTypeHash;
    emitTypeHash = [&](LValue lv, const TypePtr &t) {
      if (!t)
        return;
      if (isPtrType(t))
        return; // deferred to rewriteExitToCrc32Checksum
      if (isScalarType(t)) {
        emitScalarLV(std::move(lv), t);
        return;
      }
      if (std::holds_alternative<ArrayType>(t->v)) {
        const auto &at = std::get<ArrayType>(t->v);
        for (uint64_t i = 0; i < at.size; i++) {
          LValue inner = lv;
          inner.accesses.push_back(AccessIndex{Index{IntLit{(int64_t) i, {}}}, {}});
          emitTypeHash(std::move(inner), at.elem);
        }
        return;
      }
      if (std::holds_alternative<VecType>(t->v)) {
        const auto &vt = std::get<VecType>(t->v);
        for (uint64_t i = 0; i < vt.size; i++) {
          LValue inner = lv;
          inner.accesses.push_back(AccessIndex{Index{IntLit{(int64_t) i, {}}}, {}});
          emitTypeHash(std::move(inner), vt.elem);
        }
        return;
      }
      if (std::holds_alternative<StructType>(t->v)) {
        const std::string &sname = std::get<StructType>(t->v).name.name;
        if (const StructDecl *sd = findStruct(sname)) {
          for (const auto &f: sd->fields) {
            LValue inner = lv;
            inner.accesses.push_back(AccessField{f.name, {}});
            emitTypeHash(std::move(inner), f.type);
          }
        }
        return;
      }
    };

    // Every var — let-init locals AND function parameters — feeds the
    // checksum. vars.vars contains both (params have `isParam == true`
    // and the generator places them last but they take the same code
    // path here).
    for (const auto &v: vars.vars) {
      emitTypeHash(localLV(v.name), v.type);
    }

    return instrs;
  }

  // ---------------------------------------------------------------------------
  // genFunction
  // ---------------------------------------------------------------------------

  FuncGenResult genFunction(
      const RyCFG &cfg, const std::vector<std::string> &path, const VarCatalogue &vars,
      const FuncGenConfig &fcfg
  ) {
    std::mt19937 rng(fcfg.seed);

    SymCounter sym;
    sym.coefLo = fcfg.coefLo;
    sym.coefHi = fcfg.coefHi;
    sym.valueLo = fcfg.valueLo;
    sym.valueHi = fcfg.valueHi;
    sym.indexLo = fcfg.indexLo;
    sym.indexHi = fcfg.indexHi;

    // [v0.2.2] Track which (intrinsic, bitwidth) pairs are used during
    // expression generation so we can emit IntrinsicDecl entries into the
    // Program.  Make a mutable copy of fcfg.exprCfg so we can attach the
    // tracking set without mutating the caller's config.
    std::set<IntrinsicUseKey> usedIntrinsics;
    ExprGenConfig exprCfg = fcfg.exprCfg;
    if (fcfg.enableIntrinsics) {
      exprCfg.usedIntrinsics = &usedIntrinsics;
    } else {
      exprCfg.enableIntrinsics = false;
      exprCfg.usedIntrinsics = nullptr;
    }

    // Off-path volume scaling (see FuncGenConfig::offPathMultiplier): the
    // CLI volume knobs describe on-path blocks; off-path blocks — which the
    // solver never visits — get them scaled, widening the compiler-facing
    // surface at zero solver cost.
    auto scaleOff = [&](int v) { return (int) std::lround(v * fcfg.offPathMultiplier); };
    int offNStmts = std::max(0, scaleOff(fcfg.nStmts));
    ExprGenConfig offExprCfg = exprCfg;
    offExprCfg.minAtoms = std::max(1, scaleOff(exprCfg.minAtoms));
    offExprCfg.maxAtoms = std::max(offExprCfg.minAtoms, scaleOff(exprCfg.maxAtoms));

    // Generate a single input sym (i32) used for interest-init requires
    std::string inputSym = sym.nextValue();

    std::unordered_set<std::string> pathSet(path.begin(), path.end());
    auto cfgLabels = cfg.labels();

    std::vector<Block> blocks;
    blocks.reserve(cfgLabels.size());

    for (const auto &blkLabel: cfgLabels) {
      const auto *blk = cfg.get(blkLabel);
      bool onPath = pathSet.count(blkLabel) > 0;
      bool isExit = (blkLabel == cfg.exitLabel);

      Block block;
      block.label = BlockLabel{"^" + blkLabel, {}};

      int coefsBefore = sym.countOfKind(SymKind::Coef);

      if (isExit) {
        // Exit block: compute checksum and return. Pointer loads are
        // deferred to rewriteExitToCrc32Checksum so the SUM the solver
        // sees stays free of `load %p` constraints.
        auto chkInstrs = buildSumChecksum(vars);
        for (auto &ci: chkInstrs)
          block.instrs.push_back(std::move(ci));

        Expr retVal = simpleExpr(rvalAtom(localLV("%_chk")));
        block.term = Terminator{RetTerm{std::move(retVal), {}}};

      } else {
        // Non-exit block: generate statements

        // Special entry block setup: assign ptr vars and interest-init require
        if (blkLabel == cfg.entry) {
          // Assign all ptr vars (addr or addr-of-ptr)
          for (const auto &v: vars.vars) {
            if (!isPtrType(v.type))
              continue;
            if (!v.ptrTarget)
              continue;

            // %p = addr %target
            LValue lhs = localLV(v.name);
            Expr rhs = simpleExpr(Atom{AddrAtom{localLV(*v.ptrTarget), {}}, {}});
            block.instrs.push_back(Instr{AssignInstr{std::move(lhs), std::move(rhs), {}}});
          }

          // Interest-init: require inputSym != 0
          if (fcfg.enableInterestInits) {
            RequireInstr req;
            req.cond.lhs = simpleExpr(coefAtom(LocalOrSymId{SymId{inputSym, {}}}));
            req.cond.op = RelOp::NE;
            req.cond.rhs = simpleExpr(coefAtom(IntLit{0, {}}));
            req.message = "nonzero input";
            block.instrs.push_back(Instr{std::move(req)});
          }
        }

        // Generate statements for on-path non-exit blocks
        if (onPath) {
          auto stmts = genBlockStmts(rng, &sym, vars, fcfg.nStmts, true, exprCfg);
          for (auto &s: stmts)
            block.instrs.push_back(std::move(s));
        } else {
          // Off-path: concrete-only stmts at the scaled volume
          auto stmts = genBlockStmts(rng, nullptr, vars, offNStmts, false, offExprCfg);
          for (auto &s: stmts)
            block.instrs.push_back(std::move(s));
        }

        // Interest coef requires (on-path only, before terminator)
        if (fcfg.enableInterestCoefs && onPath) {
          auto reqs =
              interestCoefRequires(rng, sym, coefsBefore, fcfg.pLargeCoef, fcfg.largeCoefThreshold);
          for (auto &r: reqs)
            block.instrs.push_back(std::move(r));
        }

        // Terminator
        if (blk->isBranch()) {
          Cond cond =
              genCond(rng, onPath ? &sym : nullptr, vars, onPath, onPath ? exprCfg : offExprCfg);
          BrTerm br;
          br.cond = std::move(cond);
          br.thenLabel = BlockLabel{"^" + blk->succs[0], {}};
          br.elseLabel = BlockLabel{"^" + blk->succs[1], {}};
          br.dest = br.thenLabel;
          br.isConditional = true;
          block.term = Terminator{std::move(br)};
        } else if (blk->isGoto()) {
          BrTerm br;
          br.dest = BlockLabel{"^" + blk->succs[0], {}};
          br.thenLabel = br.dest;
          br.elseLabel = br.dest;
          br.isConditional = false;
          block.term = Terminator{std::move(br)};
        } else {
          // Dead-end non-exit (shouldn't happen in well-formed CFG)
          Expr zero = simpleExpr(coefAtom(IntLit{0, {}}));
          block.term = Terminator{RetTerm{std::move(zero), {}}};
        }
      }

      blocks.push_back(std::move(block));
    }

    // ---------------------------------------------------------------------------
    // Assemble Program
    // ---------------------------------------------------------------------------

    Program prog;
    prog.structs = vars.structDecls;

    // [v0.2.2] Emit IntrinsicDecl entries for each (kind, bitwidth) pair
    // used during expression generation. The semchecker now supports
    // overloaded intrinsics with different signatures, so the same intrinsic
    // may appear at multiple bitwidths (e.g. @popcount i32 / i64).
    if (fcfg.enableIntrinsics)
      appendUsedIntrinsicDecls(usedIntrinsics, prog.intrinsics);

    FunDecl fun;
    fun.name = GlobalId{"@" + fcfg.funcName, {}};
    fun.retType = makeI32();
    fun.syms = sym.makeDecls();

    // [v0.2.2] Split VarCatalogue entries: isParam → FunDecl.params,
    // others → FunDecl.lets. Parameters carry no initializer (the
    // solver synthesises their values and the SOLVED header records
    // them; symiri replays them as positional CLI args).
    for (const auto &v: vars.vars) {
      if (v.isParam) {
        ParamDecl pd;
        pd.name = LocalId{v.name, {}};
        pd.type = v.type;
        fun.params.push_back(std::move(pd));
        continue;
      }
      LetDecl let;
      let.isMutable = true;
      let.name = LocalId{v.name, {}};
      let.type = v.type;

      if (isPtrType(v.type)) {
        // Ptr vars assigned in entry block via addr; declare as undef.
        InitVal iv;
        iv.kind = InitVal::Kind::Undef;
        let.init = std::move(iv);
      } else {
        let.init = makeInitVal(rng, v.type, vars.structDecls);
      }

      fun.lets.push_back(std::move(let));
    }

    // %_chk: let mut %_chk: i32 = 0;
    {
      LetDecl let;
      let.isMutable = true;
      let.name = LocalId{"%_chk", {}};
      let.type = makeI32();
      InitVal iv;
      iv.kind = InitVal::Kind::Int;
      iv.value = IntLit{0, {}};
      let.init = std::move(iv);
      fun.lets.push_back(std::move(let));
    }

    fun.blocks = std::move(blocks);
    prog.funs.push_back(std::move(fun));

    // Build path labels with ^ prefix
    std::vector<std::string> pathLabels;
    pathLabels.reserve(path.size());
    for (const auto &lbl: path)
      pathLabels.push_back("^" + lbl);

    return FuncGenResult{std::move(prog), std::move(pathLabels)};
  }

} // namespace refractir::reify
