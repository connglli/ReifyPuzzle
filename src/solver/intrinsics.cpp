// [v0.2.2] Solver-side SMT lowering for built-in intrinsics.
//
// This file is the single source of truth for every intrinsic
// supported by the SymIR symbolic executor / SMT constraint generator.
// To add a new intrinsic:
//   1. Add its `intrinsic @name(...)` declaration to the test / user
//      program.
//   2. Add a branch in callBuiltinIntrinsicSMT below.
//   3. Add the matching branches in:
//        src/interp/intrinsics.cpp        (interpreter concrete evaluation)
//        src/backend/intrinsics_c.cpp     (C code-gen helper)
//        src/backend/intrinsics_wasm.cpp  (WASM code-gen helper)
//
// Currently supported (§12):
//   @abs(x)      – ITE(x >= 0, x, -x); UB guard: x != INT_MIN_N
//   @min(a, b)   – ITE(a <= b, a, b) using BV_SLE
//   @max(a, b)   – ITE(a >= b, a, b) using BV_SGE
//   @popcount(x) – sum of N 1-bit extracts zero-extended to N bits
//   @clz(x)      – ITE chain over bit positions; UB guard: x != 0
//   @ctz(x)      – ITE chain over bit positions; UB guard: x != 0

#include "solver/solver.hpp"

#include <stdexcept>
#include "analysis/type_utils.hpp"

namespace symir {

  SymbolicExecutor::SymbolicValue SymbolicExecutor::callBuiltinIntrinsicSMT(
      const IntrinsicDecl &intr, std::vector<SymbolicValue> &argVals, smt::ISolver &solver,
      std::vector<smt::Term> &pc
  ) {
    uint32_t N = 32;
    if (auto pb = TypeUtils::getIntBitWidth(intr.retType))
      N = *pb;
    auto bvN = solver.make_bv_sort(N);
    const std::string &nm = intr.name.name;

    // ── @abs ─────────────────────────────────────────────────────────────
    if (nm == "@abs") {
      smt::Term x = argVals[0].term;
      // UB: x == INT_MIN_N.
      int64_t int_min_N = (N == 64) ? INT64_MIN : -(INT64_C(1) << (N - 1));
      auto minT = solver.make_bv_value_int64(bvN, int_min_N);
      pc.push_back(solver.make_term(smt::Kind::DISTINCT, {x, minT}));
      auto zero = solver.make_bv_value_int64(bvN, 0);
      auto cond = solver.make_term(smt::Kind::BV_SGE, {x, zero});
      auto neg = solver.make_term(smt::Kind::BV_NEG, {x});
      auto res = solver.make_term(smt::Kind::ITE, {cond, x, neg});
      return SymbolicValue(SymbolicValue::Kind::Int, res, solver.make_true());
    }

    // ── @min ─────────────────────────────────────────────────────────────
    if (nm == "@min") {
      smt::Term a0 = argVals[0].term;
      smt::Term a1 = argVals[1].term;
      auto cond = solver.make_term(smt::Kind::BV_SLE, {a0, a1});
      auto res = solver.make_term(smt::Kind::ITE, {cond, a0, a1});
      return SymbolicValue(SymbolicValue::Kind::Int, res, solver.make_true());
    }

    // ── @max ─────────────────────────────────────────────────────────────
    if (nm == "@max") {
      smt::Term a0 = argVals[0].term;
      smt::Term a1 = argVals[1].term;
      auto cond = solver.make_term(smt::Kind::BV_SGE, {a0, a1});
      auto res = solver.make_term(smt::Kind::ITE, {cond, a0, a1});
      return SymbolicValue(SymbolicValue::Kind::Int, res, solver.make_true());
    }

    // ── @popcount ────────────────────────────────────────────────────────
    if (nm == "@popcount") {
      smt::Term x = argVals[0].term;
      // popcount = sum of bits. Sum each bit extracted as 1-bit BV
      // zero-extended to N bits, then BV_ADD.
      smt::Term acc = solver.make_bv_value_int64(bvN, 0);
      for (uint32_t k = 0; k < N; ++k) {
        auto bit = solver.make_term(smt::Kind::BV_EXTRACT, {x}, {k, k});
        smt::Term ext =
            (N == 1) ? bit : solver.make_term(smt::Kind::BV_ZERO_EXTEND, {bit}, {N - 1});
        acc = solver.make_term(smt::Kind::BV_ADD, {acc, ext});
      }
      return SymbolicValue(SymbolicValue::Kind::Int, acc, solver.make_true());
    }

    // ── @clz / @ctz ──────────────────────────────────────────────────────
    if (nm == "@clz" || nm == "@ctz") {
      smt::Term x = argVals[0].term;
      // UB: x == 0.
      auto zero = solver.make_bv_value_int64(bvN, 0);
      pc.push_back(solver.make_term(smt::Kind::DISTINCT, {x, zero}));

      // ITE chain: walk bit positions and pick the first set bit.
      // For @clz: result == i iff bit (N-1-i) == 1 and all bits
      //   N-1 .. N-i are 0. Iterate i = 0 .. N-1; first match wins.
      // For @ctz: result == i iff bit i == 1 and bits 0 .. i-1 are 0.
      // We build from the back: start with a default value of N (never
      // reached because x != 0), then wrap with ITE for each position.
      smt::Term defaultT = solver.make_bv_value_int64(bvN, N);
      smt::Term result = defaultT;
      auto one1 = solver.make_bv_value_int64(solver.make_bv_sort(1), 1);
      if (nm == "@clz") {
        // Iterate i = N-1 down to 0; ITE(bit(N-1-i) == 1, i, result).
        for (int i = (int) N - 1; i >= 0; --i) {
          int bitPos = (int) N - 1 - i;
          auto bit =
              solver.make_term(smt::Kind::BV_EXTRACT, {x}, {(uint32_t) bitPos, (uint32_t) bitPos});
          auto cond = solver.make_term(smt::Kind::EQUAL, {bit, one1});
          auto iT = solver.make_bv_value_int64(bvN, i);
          result = solver.make_term(smt::Kind::ITE, {cond, iT, result});
        }
      } else {
        // @ctz: iterate i = N-1 down to 0; ITE(bit(i) == 1, i, result).
        for (int i = (int) N - 1; i >= 0; --i) {
          auto bit = solver.make_term(smt::Kind::BV_EXTRACT, {x}, {(uint32_t) i, (uint32_t) i});
          auto cond = solver.make_term(smt::Kind::EQUAL, {bit, one1});
          auto iT = solver.make_bv_value_int64(bvN, i);
          result = solver.make_term(smt::Kind::ITE, {cond, iT, result});
        }
      }
      return SymbolicValue(SymbolicValue::Kind::Int, result, solver.make_true());
    }

    throw std::runtime_error("Solver: unknown intrinsic " + nm);
  }

} // namespace symir
