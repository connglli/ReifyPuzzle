#pragma once

#include <iosfwd>
#include <list>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>
#include "ast/ast.hpp"
#include "interp/memory.hpp"
#include "interp/type_layout.hpp"
#include "interp/value.hpp"

namespace refractir {

  /**
   * A concrete interpreter for the RefractIR language.
   * Executes RefractIR programs by evaluating expressions and instructions
   * against concrete values for symbolic variables.
   */
  class Interpreter {
  public:
    explicit Interpreter(const Program &prog);
    // Output sink for the `Result:` line and the `--dump-exec` trace.
    // Defaults to std::cout via the single-arg ctor; callers that need to
    // capture the output (e.g. the reify oracle) pass a local stream
    // instead of redirecting the process-global std::cout, which is unsafe
    // when other threads are running concurrently.
    Interpreter(const Program &prog, std::ostream &out);
    /**
     * Executes the specified entry function with given symbolic bindings.
     * @param entryFuncName The name of the function to start execution from.
     * @param symBindings Mapping of symbolic identifiers to concrete values.
     * @param dumpExec Whether to print execution trace to stderr.
     */
    using SymBindings = std::unordered_map<std::string, std::variant<std::int64_t, double>>;

    // [v0.2.2] `paramArgs` supplies one decimal-int / hex-float string
    // per parameter of the entry function. Empty when the entry is
    // parameterless (the common case).
    void
    run(const std::string &entryFuncName, const SymBindings &symBindings,
        const std::vector<std::string> &paramArgs = {}, bool dumpExec = false);

    // RuntimeValue and Store are defined in interp/value.hpp at namespace
    // scope so the memory / type-layout collaborators can share them.

  private:
    const Program &prog_;
    std::ostream &out_; // sink for Result: / dump-exec (default std::cout)
    bool dumpExec_ = false;
    // Owns the struct registry + pure type-layout queries (sizeofType,
    // getCellTypeAtOffset, fieldOffset). Frame-independent; see TypeLayout.
    TypeLayout typeLayout_;

    // Memory model (flat heap, provenance objects, address map, allocator)
    // for pointer operations. Owns ObjectInfo; see Memory.
    Memory memory_;
    // typeMap_: varName → TypePtr, rebuilt at start of each execFunction call.
    // Per-frame static-type state (stays on the interpreter, not in Memory).
    std::unordered_map<std::string, TypePtr> typeMap_;

    TypePtr getLValueType(const LValue &lv) const;
    TypePtr getCoefType(const Coef &coef) const;
    TypePtr getAtomType(const Atom &atom) const;
    TypePtr getExprType(const Expr &expr) const;

    // --- Runtime evaluation helpers ---
    RuntimeValue makeUndef(const TypePtr &t);
    RuntimeValue broadcast(const TypePtr &t, const RuntimeValue &v);
    RuntimeValue evalInit(const InitVal &iv, const TypePtr &t, const Store &store);
    std::string rvToString(const RuntimeValue &rv) const;

    void execFunction(
        const FunDecl &f, const std::vector<RuntimeValue> &args, const SymBindings &symBindings
    );

    // [v0.2.2] Nested function call (`call @fun(...)`). Evaluates the
    // callee's body with a fresh local frame and returns its `ret`
    // value. Heap state (objects, addresses) is preserved across the
    // call so pointer arguments remain valid. typeMap_ entries that
    // collide with callee parameter/local names are saved and restored.
    RuntimeValue callFunction(const FunDecl &f, std::vector<RuntimeValue> args);

    // [v0.2.2] §9.6.1 step 5: after a callee returns, refresh caller-side
    // Store entries for any local that was promoted to memory (addr was
    // taken). Reads through a pointer commit to `heap_`; without this
    // refresh, subsequent direct reads of the local from the caller's
    // Store would observe the stale pre-call value.
    void syncStoreFromHeap(Store &store);

    // [v0.2.2] Run the body of a function whose Store has already been
    // populated. Used by both execFunction (top-level) and callFunction
    // (nested). When `outRet` is non-null, the ret value is written
    // there and no "Result:" line is printed.
    void runBlocks(const FunDecl &f, Store &store, RuntimeValue *outRet);

    // [v0.2.2] Sym bindings live on the interpreter (so nested calls
    // share the same SAT model). Captured in run() / execFunction.
    const SymBindings *symBindings_ = nullptr;

    RuntimeValue evalExpr(const Expr &e, const Store &store);
    // evalAtom dispatches on the Atom variant; each alternative's evaluation
    // lives in a dedicated evalXxxAtom helper (src/interp/expr.cpp). The two
    // trivial alternatives (CoefAtom / RValueAtom) are handled inline in the
    // dispatcher and delegate straight to evalCoef / evalLValue.
    RuntimeValue evalAtom(const Atom &a, const Store &store);
    RuntimeValue evalOpAtom(const OpAtom &arg, const Store &store);
    RuntimeValue evalUnaryAtom(const UnaryAtom &arg, const Store &store);
    RuntimeValue evalSelectAtom(const SelectAtom &arg, const Store &store);
    RuntimeValue evalCmpAtom(const CmpAtom &arg, const Store &store);
    RuntimeValue evalAddrAtom(const AddrAtom &arg, const Store &store);
    RuntimeValue evalLoadAtom(const LoadAtom &arg, const Store &store);
    RuntimeValue evalPtrIndexAtom(const PtrIndexAtom &arg, const Store &store);
    RuntimeValue evalPtrFieldAtom(const PtrFieldAtom &arg, const Store &store);
    RuntimeValue evalCastAtom(const CastAtom &arg, const Store &store);
    RuntimeValue evalCallAtom(const CallAtom &arg, const Store &store);
    // [v0.2.2] Execute a built-in intrinsic. Argument values are already
    // evaluated. Result has the intrinsic's declared return bitwidth.
    // Implemented in src/interp/intrinsics.cpp — the single source of
    // truth for interpreter-side intrinsic semantics.
    RuntimeValue callIntrinsic(
        const IntrinsicDecl &intr, const std::vector<RuntimeValue> &args, SourceSpan callSpan
    );
    RuntimeValue evalCoef(const Coef &c, const Store &store);
    RuntimeValue evalSelectVal(const SelectVal &sv, const Store &store);
    RuntimeValue evalLValue(const LValue &lv, const Store &store);
    void setLValue(const LValue &lv, RuntimeValue val, Store &store);

    bool evalCond(const Cond &c, const Store &store);
  };

} // namespace refractir
