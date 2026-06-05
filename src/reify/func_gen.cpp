#include "reify/func_gen.hpp"

#include <algorithm>
#include <cassert>
#include <functional>
#include <set>
#include <unordered_set>
#include "analysis/intrinsics.hpp"
#include "reify/intrinsic_whitelist.hpp"

namespace symir::reify {

  // ---------------------------------------------------------------------------
  // Internal helpers
  // ---------------------------------------------------------------------------

  static TypePtr makeI32() {
    return std::make_shared<Type>(Type{IntType{IntType::Kind::I32, {}, {}}, {}});
  }

  // Build an integer Type from a bitwidth (8 → i8, 16 → i16, 32 → i32, 64 → i64).
  static TypePtr makeIntTypeByWidth(uint32_t bits) {
    if (bits == 32)
      return std::make_shared<Type>(Type{IntType{IntType::Kind::I32, {}, {}}, {}});
    if (bits == 64)
      return std::make_shared<Type>(Type{IntType{IntType::Kind::I64, {}, {}}, {}});
    return std::make_shared<Type>(Type{IntType{IntType::Kind::ICustom, (int) bits, {}}, {}});
  }

  static LValue localLV(const std::string &name) { return LValue{LocalId{name, {}}, {}, {}}; }

  static Atom coefAtom(Coef c) { return Atom{CoefAtom{std::move(c), {}}, {}}; }

  static Atom rvalAtom(RValue rv) { return Atom{RValueAtom{std::move(rv), {}}, {}}; }

  static Expr simpleExpr(Atom a) { return Expr{std::move(a), {}, {}}; }

  // ---------------------------------------------------------------------------
  // Unified recursive initializer
  // ---------------------------------------------------------------------------

  static InitVal makeInitVal(const TypePtr &t, const std::vector<StructDecl> &structDecls) {
    if (isIntType(t)) {
      InitVal iv;
      iv.kind = InitVal::Kind::Int;
      iv.value = IntLit{1, {}};
      return iv;
    }
    if (isFpType(t)) {
      InitVal iv;
      iv.kind = InitVal::Kind::Float;
      iv.value = FloatLit{1.0, {}};
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
        children.push_back(std::make_shared<InitVal>(makeInitVal(at.elem, structDecls)));
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
          children.push_back(std::make_shared<InitVal>(makeInitVal(field.type, structDecls)));
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
        children.push_back(std::make_shared<InitVal>(makeInitVal(vt.elem, structDecls)));
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
          auto stmts = genBlockStmts(rng, &sym, vars, fcfg.nStmts, true, false, exprCfg);
          for (auto &s: stmts)
            block.instrs.push_back(std::move(s));
        } else {
          // Off-path: concrete-only stmts
          auto stmts =
              genBlockStmts(rng, nullptr, vars, fcfg.nStmts, false, fcfg.safeOffPath, exprCfg);
          for (auto &s: stmts)
            block.instrs.push_back(std::move(s));
        }

        // Interest coef requires (on-path only, before terminator)
        if (fcfg.enableInterestCoefs && onPath) {
          auto reqs = interestCoefRequires(sym, coefsBefore);
          for (auto &r: reqs)
            block.instrs.push_back(std::move(r));
        }

        // Terminator
        if (blk->isBranch()) {
          Cond cond = genCond(rng, onPath ? &sym : nullptr, vars, onPath, exprCfg);
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
    if (fcfg.enableIntrinsics) {
      for (const auto &[kind, bits]: usedIntrinsics) {
        for (const auto &wi: getIntrinsicWhitelist()) {
          if (wi.kind != kind)
            continue;
          IntrinsicDecl id;
          id.name = GlobalId{std::string(wi.name), {}};
          id.retType = makeIntTypeByWidth(bits);
          for (int pi = 0; pi < wi.paramCount; pi++) {
            ParamDecl pd;
            pd.name = LocalId{"%x" + std::to_string(pi), {}};
            pd.type = makeIntTypeByWidth(bits);
            id.params.push_back(std::move(pd));
          }
          prog.intrinsics.push_back(std::move(id));
          break;
        }
      }
    }

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
        let.init = makeInitVal(v.type, vars.structDecls);
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

  // ---------------------------------------------------------------------------
  // Exit-block CRC32 rewriter — see include/reify/func_gen.hpp for usage
  // ---------------------------------------------------------------------------

  namespace {

    // Build a single-atom Expr — used to wrap the state/val operands as
    // Expr arguments to the @crc32_update call.
    Expr atomToExpr(Atom a) { return Expr{std::move(a), {}, {}}; }

    // Build an Expr that reads `%_chk`. Both the state arg of @crc32_update
    // and the LHS-side `%_chk = ...` reference the same accumulator local.
    Expr chkReadExpr() {
      LValue lv = LValue{LocalId{"%_chk", {}}, {}, {}};
      Atom a = Atom{RValueAtom{std::move(lv), {}}, {}};
      return atomToExpr(std::move(a));
    }

    // True when `a` is `RValueAtom(%_chk)` with no accesses — the marker we
    // use to find the running-state operand in a buildSumChecksum addition.
    bool isChkRead(const Atom &a) {
      auto *rv = std::get_if<RValueAtom>(&a.v);
      if (!rv)
        return false;
      if (rv->rval.base.name != "%_chk")
        return false;
      if (!rv->rval.accesses.empty())
        return false;
      return true;
    }

    // True when `instr` is the `%_chk = 0;` init that buildSumChecksum emits
    // before any accumulation. Matches a single-atom RHS of integer-literal
    // value 0; the init is left untouched by the rewrite.
    bool isChkInitZero(const AssignInstr &ai) {
      if (ai.lhs.base.name != "%_chk")
        return false;
      if (!ai.lhs.accesses.empty())
        return false;
      if (!ai.rhs.rest.empty())
        return false;
      auto *ca = std::get_if<CoefAtom>(&ai.rhs.first.v);
      if (!ca)
        return false;
      auto *il = std::get_if<IntLit>(&ca->coef);
      return il && il->value == 0;
    }

    // Ensure `@crc32_update(state: i32, val: i32) : i32` is declared at the
    // program level. Idempotent: only appends a new IntrinsicDecl when no
    // matching signature is already present.
    void ensureCrc32UpdateDecl(Program &prog) {
      auto makeI32Decl = []() {
        return std::make_shared<Type>(Type{IntType{IntType::Kind::I32, {}, {}}, {}});
      };
      for (const auto &id: prog.intrinsics) {
        if (id.name.name != "@crc32_update")
          continue;
        if (id.params.size() != 2)
          continue;
        auto pb1 = intBitWidth(id.params[1].type);
        if (pb1 == 32)
          return; // already declared at i32 val width
      }
      IntrinsicDecl id;
      id.name = GlobalId{"@crc32_update", {}};
      id.retType = makeI32Decl();
      ParamDecl state;
      state.name = LocalId{"%state", {}};
      state.type = makeI32Decl();
      ParamDecl val;
      val.name = LocalId{"%val", {}};
      val.type = makeI32Decl();
      id.params.push_back(std::move(state));
      id.params.push_back(std::move(val));
      prog.intrinsics.push_back(std::move(id));
    }

  } // namespace

  size_t rewriteExitToCrc32Checksum(symir::Program &prog, const std::string &funcName) {
    // 1. Find the entry function.
    FunDecl *entry = nullptr;
    std::string canonical = funcName.empty() || funcName[0] == '@' ? funcName : "@" + funcName;
    for (auto &f: prog.funs) {
      if (f.name.name == canonical) {
        entry = &f;
        break;
      }
    }
    if (!entry)
      return 0;

    // 2. Find its exit block (terminator is RetTerm). buildSumChecksum always
    // emits its chain there; if no RetTerm block exists, the function
    // wasn't shaped by genFunction and there's nothing for us to rewrite.
    Block *exit = nullptr;
    for (auto &b: entry->blocks) {
      if (std::holds_alternative<RetTerm>(b.term)) {
        exit = &b;
        break;
      }
    }
    if (!exit)
      return 0;

    // 3. Walk instructions. Skip the `%_chk = 0;` init. For each
    // `%_chk = %_chk + <atom>` (or `<atom> + %_chk` for cast-first form),
    // extract the addend and replace the RHS with
    // `call @crc32_update(%_chk, <addend>)`. Any other shape (a regular
    // assignment, a require, etc.) is left as-is — we never silently
    // drop instructions in case future versions of buildSumChecksum add
    // pre/post-amble work alongside the accumulator.
    size_t updates = 0;
    for (auto &instr: exit->instrs) {
      auto *ai = std::get_if<AssignInstr>(&instr);
      if (!ai)
        continue;
      if (ai->lhs.base.name != "%_chk" || !ai->lhs.accesses.empty())
        continue;
      if (isChkInitZero(*ai))
        continue;
      // buildSumChecksum always emits exactly one tail `+ atom`. Defensively
      // bail on any other RHS shape so we don't corrupt unrelated code.
      if (ai->rhs.rest.size() != 1)
        continue;
      if (ai->rhs.rest[0].op != AddOp::Plus)
        continue;
      Atom *addendPtr = nullptr;
      if (isChkRead(ai->rhs.first)) {
        addendPtr = &ai->rhs.rest[0].atom;
      } else if (isChkRead(ai->rhs.rest[0].atom)) {
        addendPtr = &ai->rhs.first;
      } else {
        continue;
      }

      CallAtom ca;
      ca.callee = GlobalId{"@crc32_update", {}};
      ca.args.push_back(std::make_shared<Expr>(chkReadExpr()));
      ca.args.push_back(std::make_shared<Expr>(atomToExpr(std::move(*addendPtr))));
      ai->rhs = atomToExpr(Atom{std::move(ca), {}});
      ++updates;
    }

    // 4. Append `load %p` → CRC32 steps for every pointer let with a
    // scalar non-pointer pointee. These were intentionally left out
    // of the SUM (see buildSumChecksum's comment) so the solver
    // never had to encode load-dispatch in the checksum; here we
    // splice them back in for runtime opacity. The added steps go
    // BEFORE the existing terminator (still a `ret %_chk;`) and the
    // accumulator is the same `%_chk` the chain above leaves at the
    // end. Scratch lets `%_pld_<n>` are added to the function's
    // let list for non-i32 pointees (CastAtom cannot wrap a
    // LoadAtom, so the load needs a named scratch to cast from).
    int scratchCounter = 0;
    auto makeI32Decl = []() {
      return std::make_shared<Type>(Type{IntType{IntType::Kind::I32, {}, {}}, {}});
    };
    auto chkLValue = []() { return LValue{LocalId{"%_chk", {}}, {}, {}}; };

    // Collect snapshot of ptr lets up-front; we'll push_back new
    // `%_pld_*` lets and walking the live vector while mutating it
    // would mis-iterate.
    struct PtrLetInfo {
      std::string name;
      TypePtr pointee;
    };

    std::vector<PtrLetInfo> ptrLets;
    for (const auto &let: entry->lets) {
      if (!isPtrType(let.type))
        continue;
      auto ptee = pointeeType(let.type);
      if (!ptee)
        continue;
      if (!isScalarType(ptee) || isPtrType(ptee))
        continue; // ptr-to-ptr / ptr-to-aggregate: skip
      ptrLets.push_back({let.name.name, ptee});
    }
    for (const auto &pl: ptrLets) {
      LValue pLV{LocalId{pl.name, {}}, {}, {}};
      // Build the val operand of `crc32_update`.
      Atom valAtom;
      if (isIntType(pl.pointee) && intBitWidth(pl.pointee) == 32) {
        // Direct load — already i32, no scratch needed.
        LoadAtom la;
        la.rval = std::move(pLV);
        valAtom = Atom{std::move(la), {}};
      } else {
        // Allocate a scratch let `%_pld_<n>` of the pointee type,
        // emit `%_pld_<n> = load %p;`, then pass `(%_pld_<n> as i32)`
        // as the val.
        std::string slotName = "%_pld_" + std::to_string(scratchCounter++);
        LetDecl slot;
        slot.isMutable = true;
        slot.name = LocalId{slotName, {}};
        slot.type = pl.pointee;
        slot.init = InitVal{InitVal::Kind::Undef, LocalId{}, {}};
        entry->lets.push_back(std::move(slot));
        // %_pld_n = load %p;
        {
          LoadAtom la;
          la.rval = std::move(pLV);
          AssignInstr load;
          load.lhs = LValue{LocalId{slotName, {}}, {}, {}};
          load.rhs = atomToExpr(Atom{std::move(la), {}});
          exit->instrs.push_back(Instr{std::move(load)});
        }
        // valAtom = (%_pld_n as i32)
        CastAtom cast;
        cast.src = LValue{LocalId{slotName, {}}, {}, {}};
        cast.dstType = makeI32Decl();
        valAtom = Atom{std::move(cast), {}};
      }
      // %_chk = call @crc32_update(%_chk, valAtom);
      CallAtom call;
      call.callee = GlobalId{"@crc32_update", {}};
      call.args.push_back(std::make_shared<Expr>(chkReadExpr()));
      call.args.push_back(std::make_shared<Expr>(atomToExpr(std::move(valAtom))));
      AssignInstr chkUpd;
      chkUpd.lhs = chkLValue();
      chkUpd.rhs = atomToExpr(Atom{std::move(call), {}});
      exit->instrs.push_back(Instr{std::move(chkUpd)});
      ++updates;
    }

    if (updates > 0)
      ensureCrc32UpdateDecl(prog);
    return updates;
  }

  // ---------------------------------------------------------------------------
  // Minimal CRC32 oracle — see include/reify/func_gen.hpp for usage
  // ---------------------------------------------------------------------------

  namespace {

    // Convert one solver-extracted LetExitValue into an AST InitVal of
    // the matching shape. `declType` is the let's declared type — used
    // both to pick scalar vs aggregate kind and to walk struct fields
    // in declaration order. `structs` is searched for struct field
    // ordering. Pointer / Undef entries fall back to InitVal::Undef so
    // the addr-replay in the entry block still has somewhere to write.
    InitVal letExitValueToInitVal(
        const symir::SymbolicExecutor::LetExitValue &lev, const TypePtr &declType,
        const std::vector<StructDecl> &structs
    ) {
      using LEV = symir::SymbolicExecutor::LetExitValue;
      InitVal iv;
      switch (lev.kind) {
        case LEV::Kind::Int: {
          iv.kind = InitVal::Kind::Int;
          int64_t v = 0;
          if (auto pi = std::get_if<int64_t>(&lev.scalar))
            v = *pi;
          else if (auto pd = std::get_if<double>(&lev.scalar))
            v = static_cast<int64_t>(*pd);
          iv.value = IntLit{v, {}};
          return iv;
        }
        case LEV::Kind::Float: {
          iv.kind = InitVal::Kind::Float;
          double v = 0.0;
          if (auto pd = std::get_if<double>(&lev.scalar))
            v = *pd;
          else if (auto pi = std::get_if<int64_t>(&lev.scalar))
            v = static_cast<double>(*pi);
          iv.value = FloatLit{v, {}};
          return iv;
        }
        case LEV::Kind::Array:
        case LEV::Kind::Vec: {
          TypePtr elemTy;
          if (declType && std::holds_alternative<ArrayType>(declType->v))
            elemTy = std::get<ArrayType>(declType->v).elem;
          else if (declType && std::holds_alternative<VecType>(declType->v))
            elemTy = std::get<VecType>(declType->v).elem;
          std::vector<InitValPtr> children;
          children.reserve(lev.elems.size());
          for (const auto &c: lev.elems)
            children.push_back(
                std::make_shared<InitVal>(letExitValueToInitVal(c, elemTy, structs))
            );
          iv.kind = InitVal::Kind::Aggregate;
          iv.value = std::move(children);
          return iv;
        }
        case LEV::Kind::Struct: {
          const StructDecl *sd = nullptr;
          if (declType && std::holds_alternative<StructType>(declType->v)) {
            const auto &sn = std::get<StructType>(declType->v).name.name;
            for (const auto &s: structs)
              if (s.name.name == sn) {
                sd = &s;
                break;
              }
          }
          std::vector<InitValPtr> children;
          if (sd) {
            children.reserve(sd->fields.size());
            for (const auto &f: sd->fields) {
              auto it = lev.fields.find(f.name);
              if (it != lev.fields.end()) {
                children.push_back(
                    std::make_shared<InitVal>(letExitValueToInitVal(it->second, f.type, structs))
                );
              } else {
                InitVal u;
                u.kind = InitVal::Kind::Undef;
                children.push_back(std::make_shared<InitVal>(std::move(u)));
              }
            }
          }
          iv.kind = InitVal::Kind::Aggregate;
          iv.value = std::move(children);
          return iv;
        }
        case LEV::Kind::Ptr:
        case LEV::Kind::Undef:
        default:
          iv.kind = InitVal::Kind::Undef;
          return iv;
      }
    }

    // Atom-graph deep clone restricted to the variants the checksum
    // chain and the load preamble produce. Block / Atom are not
    // generically copyable (SelectAtom holds unique_ptrs), so we
    // rebuild rather than copy. Throws on any unexpected variant —
    // the caller is the exit-block-only path emitted by
    // buildSumChecksum + rewriter, which never emits SelectAtom /
    // CmpAtom / aggregates inside the accumulator chain or scratch
    // loads.
    Atom cloneChecksumAtom(const Atom &a);

    Expr cloneChecksumExpr(const Expr &e) {
      Expr ne;
      ne.first = cloneChecksumAtom(e.first);
      ne.span = e.span;
      for (const auto &tail: e.rest)
        ne.rest.push_back({tail.op, cloneChecksumAtom(tail.atom), tail.span});
      return ne;
    }

    Atom cloneChecksumAtom(const Atom &a) {
      return std::visit(
          [&a](const auto &v) -> Atom {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, CoefAtom>) {
              return Atom{CoefAtom{v.coef, v.span}, a.span};
            } else if constexpr (std::is_same_v<T, RValueAtom>) {
              return Atom{RValueAtom{v.rval, v.span}, a.span};
            } else if constexpr (std::is_same_v<T, CastAtom>) {
              return Atom{CastAtom{v.src, v.dstType, v.span}, a.span};
            } else if constexpr (std::is_same_v<T, LoadAtom>) {
              return Atom{LoadAtom{v.rval, v.span}, a.span};
            } else if constexpr (std::is_same_v<T, OpAtom>) {
              return Atom{OpAtom{v.op, v.coef, v.rval, v.span}, a.span};
            } else if constexpr (std::is_same_v<T, UnaryAtom>) {
              return Atom{UnaryAtom{v.op, v.rval, v.span}, a.span};
            } else if constexpr (std::is_same_v<T, AddrAtom>) {
              return Atom{AddrAtom{v.lv, v.span}, a.span};
            } else if constexpr (std::is_same_v<T, CallAtom>) {
              // CallAtom arg list holds shared_ptr<Expr>; sharing is
              // safe because consumers (printer, interpreter, C
              // backend) treat the AST as immutable.
              CallAtom ca;
              ca.callee = v.callee;
              ca.args = v.args;
              ca.span = v.span;
              return Atom{std::move(ca), a.span};
            } else {
              throw std::runtime_error(
                  "buildMiniCrc32Prog: unsupported atom variant in checksum chain"
              );
            }
          },
          a.v
      );
    }

  } // namespace

  symir::Program buildMiniCrc32Prog(
      const symir::Program &full, const std::string &funcName,
      const std::unordered_map<std::string, symir::SymbolicExecutor::LetExitValue> &letExitValues
  ) {
    Program minimal;
    minimal.structs = full.structs;

    std::string canonical = funcName.empty() || funcName[0] == '@' ? funcName : "@" + funcName;
    const FunDecl *entry = nullptr;
    for (const auto &f: full.funs) {
      if (f.name.name == canonical) {
        entry = &f;
        break;
      }
    }
    if (!entry)
      return minimal;

    const Block *fullExit = nullptr;
    for (const auto &b: entry->blocks) {
      if (std::holds_alternative<RetTerm>(b.term)) {
        fullExit = &b;
        break;
      }
    }
    if (!fullExit)
      return minimal;

    // Carry over only intrinsic decls referenced by the exit block.
    // The minimal program's body uses only @crc32_update; importing
    // everything else (especially randomly-generated intrinsics)
    // would demand argument lists we don't reproduce.
    for (const auto &id: full.intrinsics) {
      if (id.name.name == "@crc32_update")
        minimal.intrinsics.push_back(id);
    }

    FunDecl mini;
    mini.name = GlobalId{"@minimal_" + funcName, {}};
    mini.retType = makeI32();
    mini.params = entry->params;

    // Lets: same declarations, but scalar / aggregate inits rewritten
    // to the solver-known exit-time values. Pointer lets keep their
    // declared `undef` init — the entry block below assigns them
    // explicitly so the exit-time target replay is plain to read.
    // `%_chk` and `%_pld_*` scratch slots are left at their original
    // (already-undef / already-zero) inits — they're overwritten in
    // the exit block before being read.
    for (const auto &letd: entry->lets) {
      LetDecl newLet;
      newLet.isMutable = letd.isMutable;
      newLet.name = letd.name;
      newLet.type = letd.type;
      auto it = letExitValues.find(letd.name.name);
      if (isPtrType(letd.type) || letd.name.name == "%_chk" ||
          letd.name.name.rfind("%_pld_", 0) == 0) {
        newLet.init = letd.init;
      } else if (it != letExitValues.end() &&
                 it->second.kind != SymbolicExecutor::LetExitValue::Kind::Undef &&
                 it->second.kind != SymbolicExecutor::LetExitValue::Kind::Ptr) {
        newLet.init = letExitValueToInitVal(it->second, letd.type, full.structs);
      } else {
        newLet.init = letd.init;
      }
      mini.lets.push_back(std::move(newLet));
    }

    // Walk type `t` for a contiguous in-object offset (tag units;
    // one unit per scalar leaf — matches the solver's tag scheme)
    // and append the field / index accesses needed to reach the
    // scalar leaf at that offset. Returns true on success and
    // narrows `t` to the leaf type. False means the offset doesn't
    // land on a clean leaf — happens when the target type's leaves
    // disagree with the pointer's pointee width.
    auto findStructByName = [&](const std::string &nm) -> const StructDecl * {
      for (const auto &s: full.structs) {
        if (s.name.name == nm)
          return &s;
      }
      return nullptr;
    };
    std::function<uint64_t(const TypePtr &)> sizeUnits;
    sizeUnits = [&](const TypePtr &t) -> uint64_t {
      if (!t)
        return 1;
      if (auto at = std::get_if<ArrayType>(&t->v))
        return at->size * sizeUnits(at->elem);
      if (auto vt = std::get_if<VecType>(&t->v))
        return vt->size * sizeUnits(vt->elem);
      if (auto st = std::get_if<StructType>(&t->v)) {
        if (const auto *sd = findStructByName(st->name.name)) {
          uint64_t sum = 0;
          for (const auto &f: sd->fields)
            sum += sizeUnits(f.type);
          return sum;
        }
        return 1;
      }
      return 1; // scalar / ptr
    };
    // Type-equality probe restricted to the type families a SymIR
    // pointer pointee can name. Used to decide when to stop the
    // offset-walk: a `ptr T` should land on a sub-value of type T,
    // not on a leaf inside T.
    std::function<bool(const TypePtr &, const TypePtr &)> typeEq;
    typeEq = [&](const TypePtr &a, const TypePtr &b) -> bool {
      if (!a || !b)
        return a == b;
      if (a->v.index() != b->v.index())
        return false;
      if (auto pa = std::get_if<IntType>(&a->v)) {
        auto pb = std::get_if<IntType>(&b->v);
        if (pa->kind != pb->kind)
          return false;
        if (pa->kind == IntType::Kind::ICustom)
          return pa->bits.value_or(32) == pb->bits.value_or(32);
        return true;
      }
      if (auto pa = std::get_if<FloatType>(&a->v))
        return pa->kind == std::get<FloatType>(b->v).kind;
      if (auto pa = std::get_if<PtrType>(&a->v))
        return typeEq(pa->pointee, std::get<PtrType>(b->v).pointee);
      if (auto pa = std::get_if<ArrayType>(&a->v)) {
        auto pb = std::get_if<ArrayType>(&b->v);
        return pa->size == pb->size && typeEq(pa->elem, pb->elem);
      }
      if (auto pa = std::get_if<VecType>(&a->v)) {
        auto pb = std::get_if<VecType>(&b->v);
        return pa->size == pb->size && typeEq(pa->elem, pb->elem);
      }
      if (auto pa = std::get_if<StructType>(&a->v))
        return pa->name.name == std::get<StructType>(b->v).name.name;
      return false;
    };

    // Walk type `t` for a contiguous in-object offset (tag units;
    // one unit per scalar leaf — matches the solver's tag scheme)
    // and append the field / index accesses needed to reach a
    // sub-value whose type matches `target`. Stops at the first
    // matching (type, remOff==0) pair so a `ptr [N] T` lands on the
    // whole array, not on its first scalar leaf. Returns true on
    // success and narrows `t` to the matched sub-type. False means
    // the offset doesn't land on any sub-value of the requested
    // type — happens when the pointer pointee disagrees with the
    // base local's leaf layout.
    std::function<bool(TypePtr &, uint64_t &, std::vector<Access> &, const TypePtr &)>
        resolveOffsetPath;
    resolveOffsetPath = [&](TypePtr &t, uint64_t &remOff, std::vector<Access> &acc,
                            const TypePtr &target) -> bool {
      if (!t)
        return false;
      if (remOff == 0 && typeEq(t, target))
        return true;
      if (auto at = std::get_if<ArrayType>(&t->v)) {
        uint64_t stride = sizeUnits(at->elem);
        if (stride == 0)
          return false;
        uint64_t idx = remOff / stride;
        if (idx >= at->size)
          return false;
        acc.push_back(AccessIndex{Index{IntLit{(int64_t) idx, {}}}, {}});
        remOff -= idx * stride;
        t = at->elem;
        return resolveOffsetPath(t, remOff, acc, target);
      }
      if (auto vt = std::get_if<VecType>(&t->v)) {
        uint64_t stride = sizeUnits(vt->elem);
        if (stride == 0)
          return false;
        uint64_t idx = remOff / stride;
        if (idx >= vt->size)
          return false;
        acc.push_back(AccessIndex{Index{IntLit{(int64_t) idx, {}}}, {}});
        remOff -= idx * stride;
        t = vt->elem;
        return resolveOffsetPath(t, remOff, acc, target);
      }
      if (auto st = std::get_if<StructType>(&t->v)) {
        const StructDecl *sd = findStructByName(st->name.name);
        if (!sd)
          return false;
        for (const auto &f: sd->fields) {
          uint64_t fsz = sizeUnits(f.type);
          if (remOff < fsz) {
            acc.push_back(AccessField{f.name, {}});
            t = f.type;
            return resolveOffsetPath(t, remOff, acc, target);
          }
          remOff -= fsz;
        }
        return false;
      }
      // Scalar / ptr already exhausted aggregate walks; only a
      // typeEq match at remOff==0 (handled at the top) succeeds.
      return false;
    };

    // Entry block: one `%p = addr <exit-leaf>;` per pointer let,
    // where `<exit-leaf>` is the scalar leaf reached by walking the
    // SOLVER's exit-time (targetLocal, targetOffset) into the
    // target's declared type. This is what makes the oracle agree
    // with the full program when the body retargets pointers via
    // load-through-pp / ptrfield / ptrindex / pointer arithmetic.
    // Falls back to the original entry-block addr-init when the
    // offset doesn't resolve to a clean leaf — better an
    // observable mismatch than silently dropping a real divergence.
    Block entryBlk;
    entryBlk.label = BlockLabel{"^entry", {}};
    auto findLetType = [&](const std::string &nm) -> TypePtr {
      for (const auto &l: entry->lets)
        if (l.name.name == nm)
          return l.type;
      for (const auto &p: entry->params)
        if (p.name.name == nm)
          return p.type;
      return {};
    };
    for (const auto &letd: entry->lets) {
      if (!isPtrType(letd.type))
        continue;
      std::string targetLocal;
      uint64_t targetOffset = 0;
      auto it = letExitValues.find(letd.name.name);
      if (it != letExitValues.end() &&
          it->second.kind == SymbolicExecutor::LetExitValue::Kind::Ptr &&
          !it->second.targetLocal.empty()) {
        targetLocal = it->second.targetLocal;
        targetOffset = it->second.targetOffset;
      }
      LValue addrSrc;
      bool resolved = false;
      if (!targetLocal.empty()) {
        TypePtr targetTy = findLetType(targetLocal);
        TypePtr pointeeTy = pointeeType(letd.type);
        std::vector<Access> acc;
        uint64_t remOff = targetOffset;
        TypePtr cur = targetTy;
        if (targetTy && pointeeTy && resolveOffsetPath(cur, remOff, acc, pointeeTy)) {
          addrSrc.base = LocalId{targetLocal, {}};
          addrSrc.accesses = std::move(acc);
          resolved = true;
        }
      }
      if (!resolved) {
        // Fallback: replay the original entry-block `%p = addr X;`.
        // Drops cleanly when the body never resolved %p to a single
        // object — the pointer stays at its entry-init target.
        for (const auto &b: entry->blocks) {
          if (b.label.name != "^entry")
            continue;
          for (const auto &instr: b.instrs) {
            auto *ai = std::get_if<AssignInstr>(&instr);
            if (!ai)
              continue;
            if (ai->lhs.base.name != letd.name.name || !ai->lhs.accesses.empty())
              continue;
            if (!ai->rhs.rest.empty())
              continue;
            if (auto *aa = std::get_if<AddrAtom>(&ai->rhs.first.v)) {
              addrSrc = aa->lv;
              resolved = true;
              break;
            }
          }
          break;
        }
      }
      if (!resolved)
        continue; // truly unresolvable — leave %p undef
      AssignInstr ai;
      ai.lhs = LValue{LocalId{letd.name.name, {}}, {}, {}};
      ai.rhs = Expr{Atom{AddrAtom{std::move(addrSrc), {}}, {}}, {}, {}};
      entryBlk.instrs.push_back(Instr{std::move(ai)});
    }
    BrTerm br;
    br.dest = BlockLabel{"^exit", {}};
    br.thenLabel = br.dest;
    br.elseLabel = br.dest;
    br.isConditional = false;
    entryBlk.term = Terminator{std::move(br)};
    mini.blocks.push_back(std::move(entryBlk));

    // Exit block: verbatim clone of the full's exit (the load
    // preamble, the `%_chk = 0;` init, the @crc32_update chain, and
    // `ret %_chk;`). The chain reads exactly the same lets / pointer
    // loads the full program reads, so the only thing that drives
    // its value is the lets' exit-time inits + the pointer targets
    // we just replayed.
    Block exitBlk;
    exitBlk.label = fullExit->label;
    for (const auto &instr: fullExit->instrs) {
      auto *ai = std::get_if<AssignInstr>(&instr);
      if (!ai)
        continue;
      AssignInstr cloned;
      cloned.lhs = ai->lhs;
      cloned.rhs = cloneChecksumExpr(ai->rhs);
      cloned.span = ai->span;
      exitBlk.instrs.push_back(Instr{std::move(cloned)});
    }
    if (auto *rt = std::get_if<RetTerm>(&fullExit->term)) {
      RetTerm nrt;
      if (rt->value)
        nrt.value = cloneChecksumExpr(*rt->value);
      nrt.span = rt->span;
      exitBlk.term = Terminator{std::move(nrt)};
    }
    exitBlk.span = fullExit->span;
    mini.blocks.push_back(std::move(exitBlk));

    minimal.funs.push_back(std::move(mini));
    return minimal;
  }

} // namespace symir::reify
