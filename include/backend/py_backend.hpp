#pragma once

#include <iostream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include "analysis/cfg.hpp"
#include "analysis/structurizer.hpp"
#include "ast/ast.hpp"

namespace refractir {

  /**
   * Generates Python code from a RefractIR program.
   *
   * Python has no goto and no labeled break, so this backend only
   * accepts reducible CFGs (the driver registers ReducibilityCheck for
   * --target python): each function is structured via
   * Structurizer::build + StructuredLowering::run and printed as
   * genuine while/if statements — the backend itself is a thin syntax
   * printer over the lowered ControlTree.
   *
   * Semantics: RefractIR's strict UB is checked eagerly at runtime by
   * a small helper preamble (RefractIRTrap): checked signed iN
   * arithmetic (Python ints are unbounded, so overflow must trap
   * explicitly), truncating division/remainder (Python's floor `//`
   * and `%` must not be used raw), per-operation f32 rounding via a
   * struct pack/unpack round-trip, and finiteness checks after every
   * FP operation. i1 uses the spec's canonical {0, -1} values.
   *
   * Symbols lower to `<fun>__<sym>()` provider calls that the
   * embedding (e.g. the test driver) must define in the module
   * globals before invoking the entry function.
   */
  class PyBackend {
  public:
    explicit PyBackend(std::ostream &out) : out_(out) {}

    void emit(const Program &prog);

    void setNoRequire(bool val) { noRequire_ = val; }

    void setNoMainMangle(bool val) { noMainMangle_ = val; }

  private:
    std::ostream &out_;
    bool noRequire_ = false;
    bool noMainMangle_ = false;
    const Program *prog_ = nullptr;
    std::string curFuncName_;
    int indent_ = 0;
    int stmtCount_ = 0; // statements in the innermost open suite ("pass" insertion)
    std::unordered_map<std::string, TypePtr> varTypes_;
    std::unordered_map<std::string, std::string> pyNames_; // sigiled name -> python identifier
    std::unordered_set<std::string> takenNames_;

    // --- Statement emission (src/backend/py_backend.cpp) ---
    void line(const std::string &s);
    void comment(const std::string &s);

    // Runs `body` one indent level deeper; emits `pass` if it produced
    // no statements.
    template<typename Fn>
    void suite(Fn &&body) {
      int saved = stmtCount_;
      stmtCount_ = 0;
      ++indent_;
      body();
      if (stmtCount_ == 0)
        line("pass");
      --indent_;
      stmtCount_ = saved + 1;
    }

    void emitFunction(const FunDecl &f);
    void emitLet(const LetDecl &l);
    void
    emitNode(const ControlTree &tree, const ControlTree::Node &n, const FunDecl &f, const CFG &cfg);
    void emitBlockInstrs(const Block &b);
    std::string condText(const FunDecl &f, std::size_t block, bool negate);

    // --- Expression emission (src/backend/py_expr.cpp) ---
    std::string exprStr(const Expr &expr);
    std::string atomStr(const Atom &atom);
    std::string opAtomStr(const OpAtom &arg);
    std::string selectAtomStr(const SelectAtom &arg);
    std::string cmpAtomStr(const CmpAtom &arg);
    std::string castAtomStr(const CastAtom &arg);
    std::string callAtomStr(const CallAtom &arg);
    std::string condStr(const Cond &cond);
    std::string coefStr(const Coef &coef);
    std::string lvalueStr(const LValue &lv);
    std::string selectValStr(const SelectVal &sv);

    // --- Type queries (src/backend/py_types.cpp) ---
    TypePtr getLValueType(const LValue &lv);
    TypePtr getAtomType(const Atom &atom);
    TypePtr getExprType(const Expr &expr);
    TypePtr getCoefType(const Coef &coef);
    TypePtr getSelectValType(const SelectVal &sv);
    // Bit width of an integer type (0 if not an integer type).
    static std::uint32_t intWidth(const TypePtr &t);
    // Bit width of a float type (0 if not a float type).
    static std::uint32_t floatWidth(const TypePtr &t);
    // Rejects types the python target cannot express yet.
    void requireSupportedType(const TypePtr &t, const char *what) const;

    // --- Naming ---
    std::string mangleFun(const std::string &name) const;
    std::string symCall(const std::string &symName) const;
    std::string pyLocal(const std::string &sigiled);
    void buildNameMap(const FunDecl &f, const ControlTree &tree);
    static std::string stripSigil(const std::string &name);
  };

} // namespace refractir
