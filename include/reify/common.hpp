#pragma once

// [v0.2.2] Small shared helpers used by both rysmith and rylink.  Each
// utility here is generator policy — choices the *tools* need to make
// when driving the deterministic backends.  The C / WASM backends
// themselves are deterministic; randomness lives on this side so a
// multi-program sweep can vary backend strategies independently.
//
// Header-only by convention; everything must be `inline` (or a
// template) so this file can be included from rysmith, rylink, and
// any future generator without dragging in a `.cpp`.

#include <memory>
#include <random>
#include <string>
#include <vector>
#include "ast/ast.hpp"

namespace symir::reify {

  /**
   * Resolve a `--vec-lowering` CLI choice into a concrete strategy
   * name for the C backend.  If `requested == "random"`, picks one of
   * `{vecext, scalars, array, structscalars, structarray}` uniformly
   * via `rng`; otherwise returns `requested` verbatim so the caller
   * can hand it straight to `makeVecLowering`.  Shared between
   * rysmith and rylink so both tools sweep the same strategy set with
   * the same odds.
   */
  inline std::string pickVecLowering(std::mt19937 &rng, const std::string &requested) {
    if (requested != "random")
      return requested;
    static const char *strategies[] = {
        "vecext", "scalars", "array", "structscalars", "structarray"
    };
    std::uniform_int_distribution<int> d(0, 4);
    return strategies[d(rng)];
  }

  inline FunDecl buildMainFunction(
      const FunDecl &entryFn, const std::vector<std::string> &paramValues,
      const std::string &retValue
  ) {
    FunDecl mainFn;
    mainFn.name = GlobalId{"@main", {}};
    mainFn.retType = std::make_shared<Type>(Type{IntType{IntType::Kind::I32, {}, {}}, {}});

    // 1. Declare %r: entryFn.retType
    LetDecl letR;
    letR.isMutable = true;
    letR.name = LocalId{"%r", {}};
    letR.type = entryFn.retType;
    letR.init = InitVal{InitVal::Kind::Undef, LocalId{}, {}};
    mainFn.lets.push_back(std::move(letR));

    // 2. Declare %exit_code: i32
    LetDecl letExit;
    letExit.isMutable = true;
    letExit.name = LocalId{"%exit_code", {}};
    letExit.type = std::make_shared<Type>(Type{IntType{IntType::Kind::I32, {}, {}}, {}});
    letExit.init = InitVal{InitVal::Kind::Undef, LocalId{}, {}};
    mainFn.lets.push_back(std::move(letExit));

    // 3. Construct basic block
    Block b;
    b.label = BlockLabel{"^entry", {}};

    // 3.1 Construct CallAtom
    CallAtom ca;
    ca.callee = entryFn.name;
    for (size_t i = 0; i < entryFn.params.size() && i < paramValues.size(); ++i) {
      const auto &p = entryFn.params[i];
      const std::string &valStr = paramValues[i];
      Expr argExpr;
      if (p.type && std::holds_alternative<FloatType>(p.type->v)) {
        argExpr.first = Atom{CoefAtom{Coef{FloatLit{parseFloatLiteral(valStr), {}}}, {}}, {}};
      } else {
        argExpr.first = Atom{CoefAtom{Coef{IntLit{parseIntegerLiteral(valStr), {}}}, {}}, {}};
      }
      ca.args.push_back(std::make_shared<Expr>(std::move(argExpr)));
    }

    // 3.2 Assign instruction: %r = call @entry_func(...)
    AssignInstr callAssign;
    callAssign.lhs = LValue{LocalId{"%r", {}}, {}, {}};
    callAssign.rhs = Expr{Atom{ca, {}}, {}, {}};
    b.instrs.push_back(std::move(callAssign));

    // 3.3 Comparison condition and SelectAtom
    // %exit_code = select %r == expected, 0, 1;
    Cond cond;
    cond.lhs = Expr{Atom{RValueAtom{RValue{LocalId{"%r", {}}, {}, {}}, {}}, {}}, {}, {}};
    cond.op = RelOp::EQ;
    if (entryFn.retType && std::holds_alternative<FloatType>(entryFn.retType->v)) {
      cond.rhs =
          Expr{Atom{CoefAtom{Coef{FloatLit{parseFloatLiteral(retValue), {}}}, {}}, {}}, {}, {}};
    } else {
      cond.rhs =
          Expr{Atom{CoefAtom{Coef{IntLit{parseIntegerLiteral(retValue), {}}}, {}}, {}}, {}, {}};
    }

    SelectAtom sa;
    sa.cond = std::make_unique<Cond>(std::move(cond));
    sa.vtrue = Coef{IntLit{0, {}}};
    sa.vfalse = Coef{IntLit{1, {}}};

    // Assign instruction: %exit_code = select ...
    AssignInstr selectAssign;
    selectAssign.lhs = LValue{LocalId{"%exit_code", {}}, {}, {}};
    selectAssign.rhs = Expr{Atom{std::move(sa), {}}, {}, {}};
    b.instrs.push_back(std::move(selectAssign));

    // 3.4 Terminator: ret %exit_code;
    RetTerm ret;
    ret.value = Expr{Atom{RValueAtom{RValue{LocalId{"%exit_code", {}}, {}, {}}, {}}, {}}, {}, {}};
    b.term = std::move(ret);

    b.span = {};
    mainFn.blocks.push_back(std::move(b));
    return mainFn;
  }

} // namespace symir::reify
