// [v0.2.2] Interpreter-side built-in intrinsic dispatch.
//
// This file is the single source of truth for every intrinsic
// supported by the SymIR interpreter.  To add a new intrinsic:
//   1. Add its `intrinsic @name(...)` declaration to the test / user
//      program.
//   2. Add a branch in Interpreter::callIntrinsic below.
//   3. Add the matching branches in:
//        src/solver/intrinsics.cpp      (SMT lowering)
//        src/backend/intrinsics_c.cpp   (C code-gen helper)
//        src/backend/intrinsics_wasm.cpp (WASM code-gen helper)
//
// Currently supported (§12):
//   @abs(x)      – signed absolute value; UB when x == INT_MIN_N
//   @min(a, b)   – signed minimum
//   @max(a, b)   – signed maximum
//   @popcount(x) – population count; UB when result > signed max of iN
//   @clz(x)      – count leading zeros;  UB when x == 0
//   @ctz(x)      – count trailing zeros; UB when x == 0

#include "interp/interpreter.hpp"

#include <cstdint>
#include <stdexcept>
#include "analysis/type_utils.hpp"
#include "error.hpp"

namespace symir {

  // [v0.2.2] Built-in intrinsics. The widening-and-mask strategy lets us
  // compute every operation at int64 width and sign-mask to N bits at the
  // end. UB-preconditions (e.g. `@abs(INT_MIN_N)`, `@ctz(0)`, `@clz(0)`)
  // throw `UndefinedBehaviorError`, which makes the path infeasible.
  Interpreter::RuntimeValue Interpreter::callIntrinsic(
      const IntrinsicDecl &intr, const std::vector<RuntimeValue> &args, SourceSpan /*callSpan*/
  ) {
    auto rbits = TypeUtils::getIntBitWidth(intr.retType);
    if (!rbits)
      throw std::runtime_error(
          "Intrinsic " + intr.name.name + " has non-integer return type (unsupported in v0.2.2)"
      );
    uint32_t N = *rbits;
    int64_t int_min_N = (N == 64) ? INT64_MIN : (-(INT64_C(1) << (N - 1)));
    int64_t mask = (N == 64) ? -1LL : ((INT64_C(1) << N) - 1);

    // Sign-extend a value to int64 based on bit N-1.
    auto sext = [&](int64_t v) -> int64_t {
      if (N == 64)
        return v;
      v &= mask;
      if (v & (INT64_C(1) << (N - 1)))
        v |= ~mask;
      return v;
    };

    // Safely fetch the i-th argument as a sign-extended int64.
    auto intVal = [&](size_t i) -> int64_t {
      if (i >= args.size())
        throw std::runtime_error("Intrinsic " + intr.name.name + ": argument count error");
      if (args[i].kind != RuntimeValue::Kind::Int)
        throw std::runtime_error("Intrinsic " + intr.name.name + ": non-integer argument");
      return sext(args[i].intVal);
    };

    RuntimeValue res;
    res.kind = RuntimeValue::Kind::Int;
    res.bits = N;

    const std::string &name = intr.name.name;

    // ── @abs ─────────────────────────────────────────────────────────────
    if (name == "@abs") {
      int64_t x = intVal(0);
      if (x == int_min_N)
        throw UndefinedBehaviorError("UB: @abs result not representable (-INT_MIN_N overflow)");
      res.intVal = sext(x < 0 ? -x : x);
      return res;
    }

    // ── @min ─────────────────────────────────────────────────────────────
    if (name == "@min") {
      int64_t a = intVal(0), b = intVal(1);
      res.intVal = sext(a < b ? a : b);
      return res;
    }

    // ── @max ─────────────────────────────────────────────────────────────
    if (name == "@max") {
      int64_t a = intVal(0), b = intVal(1);
      res.intVal = sext(a > b ? a : b);
      return res;
    }

    // ── @popcount ────────────────────────────────────────────────────────
    if (name == "@popcount") {
      uint64_t u = static_cast<uint64_t>(intVal(0)) & static_cast<uint64_t>(mask);
      int64_t c = __builtin_popcountll(u);
      // §7.7 rule 25: result must be representable in the declared iN.
      // At very narrow widths (e.g. i1, i2) the popcount can overflow.
      const int64_t signed_max = (N >= 64) ? INT64_MAX : ((INT64_C(1) << (N - 1)) - 1);
      if (c > signed_max)
        throw UndefinedBehaviorError("UB: @popcount result not representable in declared iN");
      res.intVal = sext(c);
      return res;
    }

    // ── @clz ─────────────────────────────────────────────────────────────
    if (name == "@clz") {
      uint64_t u = static_cast<uint64_t>(intVal(0)) & static_cast<uint64_t>(mask);
      if (u == 0)
        throw UndefinedBehaviorError("UB: @clz requires non-zero input (§12.2)");
      int64_t c = 0;
      for (int b = (int) N - 1; b >= 0; --b) {
        if ((u >> b) & 1ULL)
          break;
        ++c;
      }
      res.intVal = sext(c);
      return res;
    }

    // ── @ctz ─────────────────────────────────────────────────────────────
    if (name == "@ctz") {
      uint64_t u = static_cast<uint64_t>(intVal(0)) & static_cast<uint64_t>(mask);
      if (u == 0)
        throw UndefinedBehaviorError("UB: @ctz requires non-zero input (§12.2)");
      int64_t c = 0;
      for (uint32_t b = 0; b < N; ++b) {
        if ((u >> b) & 1ULL)
          break;
        ++c;
      }
      res.intVal = sext(c);
      return res;
    }

    throw std::runtime_error("Unknown intrinsic: " + intr.name.name);
  }

} // namespace symir
