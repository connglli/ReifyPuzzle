#include "reify/func_gen.hpp"

#include <algorithm>
#include <cassert>
#include <unordered_set>

namespace symir::reify {

  // ---------------------------------------------------------------------------
  // Internal helpers
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

  static Atom coefAtom(Coef c) { return Atom{CoefAtom{std::move(c), {}}, {}}; }

  static Atom rvalAtom(RValue rv) { return Atom{RValueAtom{std::move(rv), {}}, {}}; }

  static Expr simpleExpr(Atom a) { return Expr{std::move(a), {}, {}}; }

  // ---------------------------------------------------------------------------
  // InitVal builders per type
  // ---------------------------------------------------------------------------

  // Build InitVal for a scalar variable using a matching input symbol
  static InitVal makeScalarInitVal(const TypePtr &t, const std::string &inputSymName) {
    InitVal iv;
    if (isIntType(t)) {
      // Use the input sym directly if it's i32; otherwise use concrete 1
      // To avoid type mismatch, check bitwidth
      // For non-i32 types, we use concrete literal 1 and rely on entry block assignment
      iv.kind = InitVal::Kind::Int;
      iv.value = IntLit{1, {}};
      (void) inputSymName;
    } else if (isFpType(t)) {
      iv.kind = InitVal::Kind::Float;
      iv.value = FloatLit{1.0, {}};
      (void) inputSymName;
    } else {
      iv.kind = InitVal::Kind::Int;
      iv.value = IntLit{0, {}};
      (void) inputSymName;
    }
    return iv;
  }

  static InitVal makeArrayInitVal(const ArrayType &at) {
    std::vector<InitValPtr> children;
    for (uint64_t i = 0; i < at.size; i++) {
      auto child = std::make_shared<InitVal>();
      if (isIntType(at.elem)) {
        child->kind = InitVal::Kind::Int;
        child->value = IntLit{1, {}};
      } else if (isFpType(at.elem)) {
        child->kind = InitVal::Kind::Float;
        child->value = FloatLit{1.0, {}};
      } else {
        // Nested array or struct element — use 0
        child->kind = InitVal::Kind::Int;
        child->value = IntLit{0, {}};
      }
      children.push_back(child);
    }
    InitVal iv;
    iv.kind = InitVal::Kind::Aggregate;
    iv.value = std::move(children);
    return iv;
  }

  static InitVal makeStructInitVal(const StructDecl &sd) {
    std::vector<InitValPtr> children;
    for (const auto &field: sd.fields) {
      auto child = std::make_shared<InitVal>();
      if (isIntType(field.type)) {
        child->kind = InitVal::Kind::Int;
        child->value = IntLit{1, {}};
      } else if (isFpType(field.type)) {
        child->kind = InitVal::Kind::Float;
        child->value = FloatLit{1.0, {}};
      } else {
        child->kind = InitVal::Kind::Int;
        child->value = IntLit{0, {}};
      }
      children.push_back(child);
    }
    InitVal iv;
    iv.kind = InitVal::Kind::Aggregate;
    iv.value = std::move(children);
    return iv;
  }

  // ---------------------------------------------------------------------------
  // Checksum builder
  // ---------------------------------------------------------------------------

  static std::vector<Instr> buildChecksum(const VarCatalogue &vars) {
    std::vector<Instr> instrs;
    auto i32 = makeI32();

    // %_chk = 0;
    {
      Expr zero = simpleExpr(coefAtom(IntLit{0, {}}));
      instrs.push_back(Instr{AssignInstr{localLV("%_chk"), std::move(zero), {}}});
    }

    // For each var, accumulate into %_chk
    // Helper: emit `%_chk = %_chk + <atom>` or `%_chk = <castAtom> + %_chk`.
    //
    // The SIR printer renders CastAtom as `<lval> as <type>`. Due to parser grammar
    // constraints, a CastAtom can only appear as the FIRST atom of an expression —
    // `lval as type` in tail position (after +/-) is not parseable. So for non-i32
    // values we emit the cast first and add %_chk as the tail, while for plain i32
    // RValues we can keep the conventional `%_chk + <rval>` form.
    auto emitChkAccum = [&](Atom valueAtom, bool isCast) {
      Expr rhs;
      if (isCast) {
        // CastAtom first: %_chk = <cast_atom> + %_chk
        rhs.first = std::move(valueAtom);
        rhs.rest.push_back({AddOp::Plus, rvalAtom(localLV("%_chk")), {}});
      } else {
        // Plain i32 RValue: %_chk = %_chk + <rval>
        rhs.first = rvalAtom(localLV("%_chk"));
        rhs.rest.push_back({AddOp::Plus, std::move(valueAtom), {}});
      }
      instrs.push_back(Instr{AssignInstr{localLV("%_chk"), std::move(rhs), {}}});
    };

    for (const auto &v: vars.vars) {
      if (isPtrType(v.type)) {
        // Skip pointer vars (non-deterministic addresses)
        continue;
      }
      if (isScalarType(v.type)) {
        if (isIntType(v.type) && intBitWidth(v.type) == 32) {
          emitChkAccum(rvalAtom(localLV(v.name)), /*isCast=*/false);
        } else {
          // Non-i32 scalar: cast to i32
          CastAtom ca;
          ca.src = LValue{LocalId{v.name, {}}, {}, {}};
          ca.dstType = i32;
          emitChkAccum(Atom{std::move(ca), {}}, /*isCast=*/true);
        }
      } else if (std::holds_alternative<ArrayType>(v.type->v)) {
        const auto &at = std::get<ArrayType>(v.type->v);
        for (uint64_t i = 0; i < at.size; i++) {
          if (isIntType(at.elem) && intBitWidth(at.elem) == 32) {
            emitChkAccum(rvalAtom(arrayLV(v.name, (int64_t) i)), /*isCast=*/false);
          } else if (isIntType(at.elem) || isFpType(at.elem)) {
            CastAtom ca;
            ca.src = arrayLV(v.name, (int64_t) i);
            ca.dstType = i32;
            emitChkAccum(Atom{std::move(ca), {}}, /*isCast=*/true);
          }
          // else: nested agg element, skip
        }
      } else if (std::holds_alternative<StructType>(v.type->v)) {
        // Find the struct decl
        const std::string &sname = v.structTypeName;
        const StructDecl *sd = nullptr;
        for (const auto &decl: vars.structDecls)
          if (decl.name.name == sname) {
            sd = &decl;
            break;
          }
        if (!sd)
          continue;
        for (const auto &f: sd->fields) {
          if (!isScalarType(f.type))
            continue; // skip ptr/agg fields
          LValue flv;
          flv.base = LocalId{v.name, {}};
          flv.accesses.push_back(AccessField{f.name, {}});
          if (isIntType(f.type) && intBitWidth(f.type) == 32) {
            emitChkAccum(rvalAtom(std::move(flv)), /*isCast=*/false);
          } else {
            CastAtom ca;
            ca.src = std::move(flv);
            ca.dstType = i32;
            emitChkAccum(Atom{std::move(ca), {}}, /*isCast=*/true);
          }
        }
      }
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
        // Exit block: compute checksum and return
        auto chkInstrs = buildChecksum(vars);
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
          auto stmts = genBlockStmts(rng, &sym, vars, fcfg.nStmts, true, false, fcfg.exprCfg);
          for (auto &s: stmts)
            block.instrs.push_back(std::move(s));
        } else {
          // Off-path: concrete-only stmts
          auto stmts =
              genBlockStmts(rng, nullptr, vars, fcfg.nStmts, false, fcfg.safeOffPath, fcfg.exprCfg);
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
          Cond cond = genCond(rng, onPath ? &sym : nullptr, vars, onPath, fcfg.exprCfg);
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

    FunDecl fun;
    fun.name = GlobalId{"@" + fcfg.funcName, {}};
    fun.retType = makeI32();
    fun.syms = sym.makeDecls();

    // LetDecls: one per variable + %_chk
    for (const auto &v: vars.vars) {
      LetDecl let;
      let.isMutable = true;
      let.name = LocalId{v.name, {}};
      let.type = v.type;

      if (isPtrType(v.type)) {
        // Ptr vars initialized to undef (assigned in entry block)
        InitVal iv;
        iv.kind = InitVal::Kind::Undef;
        let.init = std::move(iv);
      } else if (isScalarType(v.type)) {
        let.init = makeScalarInitVal(v.type, inputSym);
      } else if (std::holds_alternative<ArrayType>(v.type->v)) {
        let.init = makeArrayInitVal(std::get<ArrayType>(v.type->v));
      } else if (std::holds_alternative<StructType>(v.type->v)) {
        const std::string &sname = v.structTypeName;
        const StructDecl *sd = nullptr;
        for (const auto &decl: vars.structDecls)
          if (decl.name.name == sname) {
            sd = &decl;
            break;
          }
        if (sd) {
          let.init = makeStructInitVal(*sd);
        } else {
          InitVal iv;
          iv.kind = InitVal::Kind::Undef;
          let.init = std::move(iv);
        }
      } else {
        InitVal iv;
        iv.kind = InitVal::Kind::Undef;
        let.init = std::move(iv);
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

} // namespace symir::reify
