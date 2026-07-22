#pragma once

// [v0.2.2] Small shared helpers used by both rysmith and rylink.  Each
// utility here is generator policy — choices the *tools* need to make
// when driving the deterministic backends.  The C / WASM backends
// themselves are deterministic; randomness lives on this side so a
// multi-program sweep can vary backend strategies independently.

#include <filesystem>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <vector>
#include "ast/ast.hpp"
#include "reify/state_profile.hpp"

namespace refractir::reify {

  /**
   * Resolve a `--vec-lowering` CLI choice into a concrete strategy
   * name for the C backend.  If `requested == "random"`, picks one of
   * `{vecext, scalars, array, structscalars, structarray}` uniformly
   * via `rng`; otherwise returns `requested` verbatim so the caller
   * can hand it straight to `makeCVecLowering`.  Shared between
   * rysmith and rylink so both tools sweep the same strategy set with
   * the same odds.
   */
  inline std::string pickVecLowering(
      std::mt19937 &rng, const std::string &requested, const std::string &target = "c"
  ) {
    if (requested != "random")
      return requested;
    if (target == "python") {
      // The python backend supports every strategy except vecext
      // (no native SIMD value type).
      static const char *pyStrategies[] = {"array", "scalars", "structscalars", "structarray"};
      std::uniform_int_distribution<int> d(0, 3);
      return pyStrategies[d(rng)];
    }
    if (target == "wasm") {
      // The python backend supports every strategy except struct-related ones
      static const char *wasmStrategies[] = {"vecext", "array", "scalars"};
      std::uniform_int_distribution<int> d(0, 2);
      return wasmStrategies[d(rng)];
    }
    static const char *strategies[] = {
        "vecext", "scalars", "array", "structscalars", "structarray"
    };
    std::uniform_int_distribution<int> d(0, 4);
    return strategies[d(rng)];
  }

  /**
   * [v0.2.3] Resolve a --structured-lowering request (true|false|random)
   * to a per-program decision. Shared between rysmith and rylink so
   * both tools flip the same coin with the same odds. Only "random"
   * consumes RNG state, so runs without the flag keep their historical
   * seed streams.
   */
  inline bool pickStructuredLowering(std::mt19937 &rng, const std::string &requested) {
    if (requested == "true")
      return true;
    if (requested != "random")
      return false;
    std::uniform_int_distribution<int> d(0, 1);
    return d(rng) == 1;
  }

  /**
   * One call site to splice into the main wrapper. `paramValues` are
   * decimal-int / hex-float strings (one per entry-function parameter,
   * in declaration order) parsed into IntLit / FloatLit atoms.
   * `retValue` is the expected return value; when non-empty the wrapper
   * compares the call result against it via `@check_chksum`. An empty
   * `retValue` skips the check for that call (used when the descriptor
   * has no oracle, e.g. symiri failed to produce one).
   */
  struct MainCall {
    std::vector<std::string> paramValues;
    std::string retValue;
  };

  /**
   * Make sure `prog.intrinsics` declares `@check_chksum(i32, i32) : i32`.
   * Idempotent — only appends a new IntrinsicDecl when no matching
   * signature is already present. Called by buildMainFunction whenever
   * any of its calls carry a non-empty retValue so the AST is
   * self-consistent before SIRPrinter runs.
   */
  inline void ensureCheckChksumDecl(Program &prog) {
    auto makeI32 = []() {
      return std::make_shared<Type>(Type{IntType{IntType::Kind::I32, {}, {}}, {}});
    };
    for (const auto &id: prog.intrinsics) {
      if (id.name.name != "@check_chksum")
        continue;
      if (id.params.size() != 2)
        continue;
      // Type widths are all required to be i32 by the semchecker, so a
      // name + arity match is sufficient.
      return;
    }
    IntrinsicDecl id;
    id.name = GlobalId{"@check_chksum", {}};
    id.retType = makeI32();
    ParamDecl pe;
    pe.name = LocalId{"%expected", {}};
    pe.type = makeI32();
    ParamDecl pa;
    pa.name = LocalId{"%actual", {}};
    pa.type = makeI32();
    id.params.push_back(std::move(pe));
    id.params.push_back(std::move(pa));
    prog.intrinsics.push_back(std::move(id));
  }

  /**
   * Build a `fun @main() : i32` that runs `entryFn` once per element of
   * `calls`, asserting each return value via `@check_chksum(EXPECTED, …)`
   * when the corresponding `retValue` is non-empty. Always returns 0 on
   * the happy path; mismatches abort inside @check_chksum's lowering
   * (fprintf + abort in C, UB in symiri).
   *
   * Side effect: appends `@check_chksum(i32, i32) : i32` to
   * `prog.intrinsics` (idempotent via ensureCheckChksumDecl) whenever
   * any call carries a non-empty retValue.
   *
   * Backward-compat overload below preserves the original
   * `(entryFn, paramValues, retValue)` shape for call sites that haven't
   * been migrated yet; it forwards to this one with a single-element
   * vector.
   */
  inline FunDecl
  buildMainFunction(Program &prog, const FunDecl &entryFn, const std::vector<MainCall> &calls) {
    auto makeI32 = []() {
      return std::make_shared<Type>(Type{IntType{IntType::Kind::I32, {}, {}}, {}});
    };

    FunDecl mainFn;
    mainFn.name = GlobalId{"@main", {}};
    mainFn.retType = makeI32();

    // `%r` is reused across every call site as a scratch holder for the
    // entry-function return value. Its declared type matches entryFn so
    // a float-returning entry doesn't trip the typechecker; the actual
    // value is consumed immediately by @check_chksum (when present) or
    // dropped.
    LetDecl letR;
    letR.isMutable = true;
    letR.name = LocalId{"%r", {}};
    letR.type = entryFn.retType;
    letR.init = InitVal{InitVal::Kind::Undef, LocalId{}, {}};
    mainFn.lets.push_back(std::move(letR));

    Block b;
    b.label = BlockLabel{"^entry", {}};

    bool anyCheck = false;
    for (const auto &call: calls) {
      // %r = call @entry(arg0, arg1, ...);
      CallAtom ca;
      ca.callee = entryFn.name;
      for (size_t i = 0; i < entryFn.params.size() && i < call.paramValues.size(); ++i) {
        const auto &p = entryFn.params[i];
        const std::string &valStr = call.paramValues[i];
        Expr argExpr;
        if (p.type && std::holds_alternative<FloatType>(p.type->v)) {
          argExpr.first = Atom{CoefAtom{Coef{FloatLit{parseFloatLiteral(valStr), {}}}, {}}, {}};
        } else {
          argExpr.first = Atom{CoefAtom{Coef{IntLit{parseIntegerLiteral(valStr), {}}}, {}}, {}};
        }
        ca.args.push_back(std::make_shared<Expr>(std::move(argExpr)));
      }
      AssignInstr callAssign;
      callAssign.lhs = LValue{LocalId{"%r", {}}, {}, {}};
      callAssign.rhs = Expr{Atom{std::move(ca), {}}, {}, {}};
      b.instrs.push_back(std::move(callAssign));

      // %r = call @check_chksum(EXPECTED, %r);  (skipped when retValue
      // is empty — happens for descriptors that the symiri-capture
      // step couldn't fill in).
      //
      // The check is gated on an integer-returning entry: @check_chksum
      // is i32-typed and RefractIR has no implicit FP↔int cast at call
      // boundaries. Float-returning entries skip the check; we
      // intentionally don't synthesise a hash-of-bits comparison here
      // because reify's float oracles already go through the
      // sum/CRC32 path on the RefractIR-side checksum machinery.
      if (!call.retValue.empty() && entryFn.retType &&
          std::holds_alternative<IntType>(entryFn.retType->v)) {
        CallAtom check;
        check.callee = GlobalId{"@check_chksum", {}};
        // arg0: expected (literal from descriptor)
        Expr expArg;
        expArg.first = Atom{CoefAtom{Coef{IntLit{parseIntegerLiteral(call.retValue), {}}}, {}}, {}};
        check.args.push_back(std::make_shared<Expr>(std::move(expArg)));
        // arg1: actual (%r read)
        Expr actArg;
        actArg.first = Atom{RValueAtom{RValue{LocalId{"%r", {}}, {}, {}}, {}}, {}};
        check.args.push_back(std::make_shared<Expr>(std::move(actArg)));
        AssignInstr checkAssign;
        checkAssign.lhs = LValue{LocalId{"%r", {}}, {}, {}};
        checkAssign.rhs = Expr{Atom{std::move(check), {}}, {}, {}};
        b.instrs.push_back(std::move(checkAssign));
        anyCheck = true;
      }
    }

    // Always exit with 0 on the happy path. Any mismatch above
    // unwinds through @check_chksum's abort() before this terminator
    // is reached.
    RetTerm ret;
    Expr zero;
    zero.first = Atom{CoefAtom{Coef{IntLit{0, {}}}, {}}, {}};
    ret.value = std::move(zero);
    b.term = std::move(ret);

    b.span = {};
    mainFn.blocks.push_back(std::move(b));

    if (anyCheck)
      ensureCheckChksumDecl(prog);

    return mainFn;
  }

  /// Single-realization convenience overload — wraps the multi-call form
  /// with a one-element vector so existing single-call sites don't have
  /// to be migrated all at once.
  inline FunDecl buildMainFunction(
      Program &prog, const FunDecl &entryFn, const std::vector<std::string> &paramValues,
      const std::string &retValue
  ) {
    return buildMainFunction(prog, entryFn, std::vector<MainCall>{{paramValues, retValue}});
  }

  // ---------------------------------------------------------------------------
  // Shared symiri runner
  // ---------------------------------------------------------------------------

  // Run frontend and analysis passes on prog. Returns true if well-formed.
  bool runAnalysisPasses(refractir::Program &prog, bool verbose);

  // Run symiri on `sirPath` with `--main <funcName>` and capture its
  // `Result: <value>` output line. Returns the trimmed value string on
  // success; std::nullopt when symiri fails to launch, exits non-zero,
  // or doesn't produce a parseable `Result:` line.
  //
  // `paramArgs` are forwarded as positional CLI args after `--` so
  // functions with parameters can be exercised deterministically.
  //
  // When `outProfile` is non-null it is filled with the per-program-point
  // StateProfile of this very run (granularity `gran`), so a caller that
  // needs both the `Result:` value and the state trace pays for a single
  // interpret instead of two — e.g. rysmith captures the rytwin profile
  // during the same run it uses to validate the emitted program.
  std::optional<std::string> runSymiriCaptureResult(
      const std::filesystem::path &sirPath, const std::string &funcName,
      const std::vector<std::string> &paramArgs, StateProfile *outProfile = nullptr,
      StateGranularity gran = StateGranularity::Pbb
  );

  // ---------------------------------------------------------------------------
  // Shared backend compiler helpers
  // ---------------------------------------------------------------------------

  // Compile a Program to C (in-process). `structuredLowering` selects
  // the goto-free while/do-while body emission; the caller must have
  // ensured the program is reducible.
  // `noUbGuards` drops the dynamic UB guards (see CBackend::setNoUbGuards);
  // sound only when the generated program is known UB-free.
  bool emitCInProcess(
      refractir::Program &prog, const std::filesystem::path &outDir, const std::string &primaryStem,
      bool keepRequire, bool noUbGuards, const std::string &vecLowering, bool structuredLowering,
      bool emitMain, bool splitBySource, bool verbose
  );

  // Compile a Program to WASM (in-process). `structuredLowering`
  // selects genuine block/loop/if emission over the $__pc dispatch
  // loop (WasmBackend::setStructuredLowering); the caller must have
  // ensured the program is reducible.
  bool emitWasmInProcess(
      refractir::Program &prog, const std::filesystem::path &outFile, bool keepRequire,
      bool noUbGuards, const std::string &vecLowering, bool structuredLowering, bool emitMain,
      bool verbose
  );

  // [v0.2.3] Compile a Program to Python (in-process). Requires a
  // reducible program (the backend structurizes unconditionally).
  // `vecLowering` names a python strategy (empty = "array"); vecext
  // and unknown names fail.
  bool emitPyInProcess(
      refractir::Program &prog, const std::filesystem::path &outFile, bool keepRequire,
      bool noUbGuards, const std::string &vecLowering, bool emitMain, bool verbose
  );

  // Parse a .sir file and compile it with the C / WASM / Python
  // backend. Returns true on success. `structuredLowering` applies to
  // the C and WASM targets (python is always structured).
  bool compileSirInProcess(
      const std::filesystem::path &sirPath, const std::string &target,
      const std::filesystem::path &outPath, bool keepRequire, bool noUbGuards,
      const std::string &vecLowering, bool structuredLowering, bool emitMain, bool verbose
  );

} // namespace refractir::reify
