// [v0.2.2] Solver-side SMT lowering for built-in intrinsics.
//
// This file is the single source of truth for every intrinsic
// supported by the SymIR symbolic executor / SMT constraint generator.
// To add a new intrinsic:
//   1. Add its IntrinsicKind to include/analysis/intrinsics.hpp.
//   2. Implement a subclass of SolverIntrinsic and register it below.
//
// Width conventions:
//   - `N` is the return-type bit-width.
//   - For predicate intrinsics (@parity, @is_pow2) the return is bv1
//     while the input is bvN — impls read per-arg widths from `intr`
//     when needed.

#include "solver/solver.hpp"

#include <functional>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include "analysis/intrinsics.hpp"
#include "analysis/type_utils.hpp"

namespace symir {

  namespace {

    /**
     * @brief Helper: param bit-width from the declaration.
     */
    inline uint32_t paramBitWidth(const IntrinsicDecl &intr, size_t i) {
      auto b = TypeUtils::getIntBitWidth(intr.params[i].type);
      if (!b)
        throw std::runtime_error(
            "Solver: intrinsic " + intr.name.name + " param " + std::to_string(i) +
            " is non-integer"
        );
      return *b;
    }

    /**
     * @brief Abstract base class for lowering built-in intrinsics to SMT terms.
     */
    class SolverIntrinsic {
    public:
      virtual ~SolverIntrinsic() = default;

      /**
       * @brief Lowers the intrinsic call into a SymbolicValue.
       * @param intr Declaration (for per-arg widths).
       * @param N Declared return bit-width.
       * @param argVals Symbolic values for all inputs.
       * @param bvN Return-side bit-vector sort of width N.
       * @param solver SMT solver backend.
       * @param pc Accumulator for path conditions / UB constraints.
       */
      virtual SymbolicExecutor::SymbolicValue solve(
          const IntrinsicDecl &intr, uint32_t N,
          const std::vector<SymbolicExecutor::SymbolicValue> &argVals, smt::Sort bvN,
          smt::ISolver &solver, std::vector<smt::Term> &pc
      ) const = 0;
    };

    // ── §12.1 Arithmetic intrinsics ─────────────────────────────────────────

    class AbsIntrinsic final : public SolverIntrinsic {
    public:
      SymbolicExecutor::SymbolicValue solve(
          const IntrinsicDecl &, uint32_t N,
          const std::vector<SymbolicExecutor::SymbolicValue> &argVals, smt::Sort bvN,
          smt::ISolver &solver, std::vector<smt::Term> &pc
      ) const override {
        smt::Term x = argVals[0].term;
        int64_t int_min_N = (N == 64) ? INT64_MIN : -(INT64_C(1) << (N - 1));
        auto minT = solver.make_bv_value_int64(bvN, int_min_N);
        pc.push_back(solver.make_term(smt::Kind::DISTINCT, {x, minT}));
        auto zero = solver.make_bv_value_int64(bvN, 0);
        auto cond = solver.make_term(smt::Kind::BV_SGE, {x, zero});
        auto neg = solver.make_term(smt::Kind::BV_NEG, {x});
        auto res = solver.make_term(smt::Kind::ITE, {cond, x, neg});
        return SymbolicExecutor::SymbolicValue(
            SymbolicExecutor::SymbolicValue::Kind::Int, res, solver.make_true()
        );
      }
    };

    class MinIntrinsic final : public SolverIntrinsic {
    public:
      SymbolicExecutor::SymbolicValue solve(
          const IntrinsicDecl &, uint32_t,
          const std::vector<SymbolicExecutor::SymbolicValue> &argVals, smt::Sort,
          smt::ISolver &solver, std::vector<smt::Term> &
      ) const override {
        auto cond = solver.make_term(smt::Kind::BV_SLE, {argVals[0].term, argVals[1].term});
        auto res = solver.make_term(smt::Kind::ITE, {cond, argVals[0].term, argVals[1].term});
        return SymbolicExecutor::SymbolicValue(
            SymbolicExecutor::SymbolicValue::Kind::Int, res, solver.make_true()
        );
      }
    };

    class MaxIntrinsic final : public SolverIntrinsic {
    public:
      SymbolicExecutor::SymbolicValue solve(
          const IntrinsicDecl &, uint32_t,
          const std::vector<SymbolicExecutor::SymbolicValue> &argVals, smt::Sort,
          smt::ISolver &solver, std::vector<smt::Term> &
      ) const override {
        auto cond = solver.make_term(smt::Kind::BV_SGE, {argVals[0].term, argVals[1].term});
        auto res = solver.make_term(smt::Kind::ITE, {cond, argVals[0].term, argVals[1].term});
        return SymbolicExecutor::SymbolicValue(
            SymbolicExecutor::SymbolicValue::Kind::Int, res, solver.make_true()
        );
      }
    };

    // ── §12.2 Bit-counting intrinsics ───────────────────────────────────────

    class PopcountIntrinsic final : public SolverIntrinsic {
    public:
      SymbolicExecutor::SymbolicValue solve(
          const IntrinsicDecl &, uint32_t N,
          const std::vector<SymbolicExecutor::SymbolicValue> &argVals, smt::Sort bvN,
          smt::ISolver &solver, std::vector<smt::Term> &
      ) const override {
        smt::Term x = argVals[0].term;
        smt::Term acc = solver.make_bv_value_int64(bvN, 0);
        for (uint32_t k = 0; k < N; ++k) {
          auto bit = solver.make_term(smt::Kind::BV_EXTRACT, {x}, {k, k});
          smt::Term ext =
              (N == 1) ? bit : solver.make_term(smt::Kind::BV_ZERO_EXTEND, {bit}, {N - 1});
          acc = solver.make_term(smt::Kind::BV_ADD, {acc, ext});
        }
        return SymbolicExecutor::SymbolicValue(
            SymbolicExecutor::SymbolicValue::Kind::Int, acc, solver.make_true()
        );
      }
    };

    class ClzIntrinsic final : public SolverIntrinsic {
    public:
      SymbolicExecutor::SymbolicValue solve(
          const IntrinsicDecl &, uint32_t N,
          const std::vector<SymbolicExecutor::SymbolicValue> &argVals, smt::Sort bvN,
          smt::ISolver &solver, std::vector<smt::Term> &pc
      ) const override {
        smt::Term x = argVals[0].term;
        auto zero = solver.make_bv_value_int64(bvN, 0);
        pc.push_back(solver.make_term(smt::Kind::DISTINCT, {x, zero}));

        smt::Term result = solver.make_bv_value_int64(bvN, N);
        auto one1 = solver.make_bv_value_int64(solver.make_bv_sort(1), 1);
        for (int i = (int) N - 1; i >= 0; --i) {
          int bitPos = (int) N - 1 - i;
          auto bit =
              solver.make_term(smt::Kind::BV_EXTRACT, {x}, {(uint32_t) bitPos, (uint32_t) bitPos});
          auto cond = solver.make_term(smt::Kind::EQUAL, {bit, one1});
          auto iT = solver.make_bv_value_int64(bvN, i);
          result = solver.make_term(smt::Kind::ITE, {cond, iT, result});
        }
        return SymbolicExecutor::SymbolicValue(
            SymbolicExecutor::SymbolicValue::Kind::Int, result, solver.make_true()
        );
      }
    };

    class CtzIntrinsic final : public SolverIntrinsic {
    public:
      SymbolicExecutor::SymbolicValue solve(
          const IntrinsicDecl &, uint32_t N,
          const std::vector<SymbolicExecutor::SymbolicValue> &argVals, smt::Sort bvN,
          smt::ISolver &solver, std::vector<smt::Term> &pc
      ) const override {
        smt::Term x = argVals[0].term;
        auto zero = solver.make_bv_value_int64(bvN, 0);
        pc.push_back(solver.make_term(smt::Kind::DISTINCT, {x, zero}));

        smt::Term result = solver.make_bv_value_int64(bvN, N);
        auto one1 = solver.make_bv_value_int64(solver.make_bv_sort(1), 1);
        for (int i = (int) N - 1; i >= 0; --i) {
          auto bit = solver.make_term(smt::Kind::BV_EXTRACT, {x}, {(uint32_t) i, (uint32_t) i});
          auto cond = solver.make_term(smt::Kind::EQUAL, {bit, one1});
          auto iT = solver.make_bv_value_int64(bvN, i);
          result = solver.make_term(smt::Kind::ITE, {cond, iT, result});
        }
        return SymbolicExecutor::SymbolicValue(
            SymbolicExecutor::SymbolicValue::Kind::Int, result, solver.make_true()
        );
      }
    };

    // ── §12.3 Integer extras (v0.2.2 extra batch A) ────────────────────────────────

    class AbsDiffIntrinsic final : public SolverIntrinsic {
    public:
      SymbolicExecutor::SymbolicValue solve(
          const IntrinsicDecl &, uint32_t,
          const std::vector<SymbolicExecutor::SymbolicValue> &argVals, smt::Sort bvN,
          smt::ISolver &solver, std::vector<smt::Term> &pc
      ) const override {
        smt::Term a = argVals[0].term, b = argVals[1].term;
        auto diffAB = solver.make_term(smt::Kind::BV_SUB, {a, b});
        auto diffBA = solver.make_term(smt::Kind::BV_SUB, {b, a});
        auto cond = solver.make_term(smt::Kind::BV_SGE, {a, b});
        auto r = solver.make_term(smt::Kind::ITE, {cond, diffAB, diffBA});
        // UB: result must be non-negative as signed iN.
        auto zero = solver.make_bv_value_int64(bvN, 0);
        pc.push_back(solver.make_term(smt::Kind::BV_SGE, {r, zero}));
        return SymbolicExecutor::SymbolicValue(
            SymbolicExecutor::SymbolicValue::Kind::Int, r, solver.make_true()
        );
      }
    };

    class SignumIntrinsic final : public SolverIntrinsic {
    public:
      SymbolicExecutor::SymbolicValue solve(
          const IntrinsicDecl &, uint32_t,
          const std::vector<SymbolicExecutor::SymbolicValue> &argVals, smt::Sort bvN,
          smt::ISolver &solver, std::vector<smt::Term> &
      ) const override {
        smt::Term x = argVals[0].term;
        auto zero = solver.make_bv_value_int64(bvN, 0);
        auto one = solver.make_bv_value_int64(bvN, 1);
        auto minus1 = solver.make_bv_value_int64(bvN, -1);
        auto isNeg = solver.make_term(smt::Kind::BV_SLT, {x, zero});
        auto isZero = solver.make_term(smt::Kind::EQUAL, {x, zero});
        auto inner = solver.make_term(smt::Kind::ITE, {isZero, zero, one});
        auto r = solver.make_term(smt::Kind::ITE, {isNeg, minus1, inner});
        return SymbolicExecutor::SymbolicValue(
            SymbolicExecutor::SymbolicValue::Kind::Int, r, solver.make_true()
        );
      }
    };

    class ClampIntrinsic final : public SolverIntrinsic {
    public:
      SymbolicExecutor::SymbolicValue solve(
          const IntrinsicDecl &, uint32_t,
          const std::vector<SymbolicExecutor::SymbolicValue> &argVals, smt::Sort,
          smt::ISolver &solver, std::vector<smt::Term> &pc
      ) const override {
        smt::Term v = argVals[0].term, lo = argVals[1].term, hi = argVals[2].term;
        pc.push_back(solver.make_term(smt::Kind::BV_SLE, {lo, hi}));
        auto vGtHi = solver.make_term(smt::Kind::BV_SGT, {v, hi});
        auto inner = solver.make_term(smt::Kind::ITE, {vGtHi, hi, v});
        auto vLtLo = solver.make_term(smt::Kind::BV_SLT, {v, lo});
        auto r = solver.make_term(smt::Kind::ITE, {vLtLo, lo, inner});
        return SymbolicExecutor::SymbolicValue(
            SymbolicExecutor::SymbolicValue::Kind::Int, r, solver.make_true()
        );
      }
    };

    class MidpointIntrinsic final : public SolverIntrinsic {
    public:
      SymbolicExecutor::SymbolicValue solve(
          const IntrinsicDecl &, uint32_t N,
          const std::vector<SymbolicExecutor::SymbolicValue> &argVals, smt::Sort bvN,
          smt::ISolver &solver, std::vector<smt::Term> &
      ) const override {
        // Sign-extend by 1 bit to avoid signed overflow on add, divide by 2
        // with bvsdiv (truncation toward zero), then extract low N bits.
        smt::Term a = argVals[0].term, b = argVals[1].term;
        auto ea = solver.make_term(smt::Kind::BV_SIGN_EXTEND, {a}, {1});
        auto eb = solver.make_term(smt::Kind::BV_SIGN_EXTEND, {b}, {1});
        auto sum = solver.make_term(smt::Kind::BV_ADD, {ea, eb});
        auto bv2 = solver.make_bv_value_int64(solver.make_bv_sort(N + 1), 2);
        auto half = solver.make_term(smt::Kind::BV_SDIV, {sum, bv2});
        auto r = solver.make_term(smt::Kind::BV_EXTRACT, {half}, {N - 1, 0});
        (void) bvN;
        return SymbolicExecutor::SymbolicValue(
            SymbolicExecutor::SymbolicValue::Kind::Int, r, solver.make_true()
        );
      }
    };

    // ── §12.4 Bit-manipulation (v0.2.2 extra batch B) ──────────────────────────────

    class ParityIntrinsic final : public SolverIntrinsic {
    public:
      SymbolicExecutor::SymbolicValue solve(
          const IntrinsicDecl &intr, uint32_t,
          const std::vector<SymbolicExecutor::SymbolicValue> &argVals, smt::Sort,
          smt::ISolver &solver, std::vector<smt::Term> &
      ) const override {
        uint32_t pN = paramBitWidth(intr, 0);
        smt::Term x = argVals[0].term;
        auto bv1 = solver.make_bv_sort(1);
        // Build XOR of all bits.
        smt::Term acc = solver.make_term(smt::Kind::BV_EXTRACT, {x}, {0, 0});
        for (uint32_t k = 1; k < pN; ++k) {
          auto bit = solver.make_term(smt::Kind::BV_EXTRACT, {x}, {k, k});
          acc = solver.make_term(smt::Kind::BV_XOR, {acc, bit});
        }
        (void) bv1;
        return SymbolicExecutor::SymbolicValue(
            SymbolicExecutor::SymbolicValue::Kind::Int, acc, solver.make_true()
        );
      }
    };

    class BswapIntrinsic final : public SolverIntrinsic {
    public:
      SymbolicExecutor::SymbolicValue solve(
          const IntrinsicDecl &, uint32_t N,
          const std::vector<SymbolicExecutor::SymbolicValue> &argVals, smt::Sort,
          smt::ISolver &solver, std::vector<smt::Term> &
      ) const override {
        smt::Term x = argVals[0].term;
        if (N == 8) {
          // 1-byte bswap is the identity.
          return SymbolicExecutor::SymbolicValue(
              SymbolicExecutor::SymbolicValue::Kind::Int, x, solver.make_true()
          );
        }
        // Concat the bytes of x in reverse order.
        uint32_t nbytes = N / 8;
        // Extract byte 0 first (lowest), then walk up; build CONCAT(high...low)
        // = byte_0, byte_1, ..., byte_{n-1}. We want byte_0 to land at the
        // highest position, so build by repeated CONCAT(prev, byte_k).
        auto result = solver.make_term(smt::Kind::BV_EXTRACT, {x}, {7, 0});
        for (uint32_t k = 1; k < nbytes; ++k) {
          uint32_t hi = (k + 1) * 8 - 1;
          uint32_t lo = k * 8;
          auto byte = solver.make_term(smt::Kind::BV_EXTRACT, {x}, {hi, lo});
          // CONCAT places first operand at the high end:
          //   result' = CONCAT(result_so_far, byte_k) ... wait, we want byte_0
          // to be the most-significant in the final. So we should build with
          // byte_0 as the LAST CONCAT'd at the LOW end. Let's invert:
          //   final = byte_0 :: byte_1 :: ... :: byte_{n-1}
          // CONCAT(a,b) puts a in high bits, b in low bits.
          // We start with byte_0 (intended high), then CONCAT(current, byte_k)
          // appends byte_k at the low end each iteration.
          result = solver.make_term(smt::Kind::BV_CONCAT, {result, byte});
        }
        return SymbolicExecutor::SymbolicValue(
            SymbolicExecutor::SymbolicValue::Kind::Int, result, solver.make_true()
        );
      }
    };

    class BitreverseIntrinsic final : public SolverIntrinsic {
    public:
      SymbolicExecutor::SymbolicValue solve(
          const IntrinsicDecl &, uint32_t N,
          const std::vector<SymbolicExecutor::SymbolicValue> &argVals, smt::Sort,
          smt::ISolver &solver, std::vector<smt::Term> &
      ) const override {
        smt::Term x = argVals[0].term;
        if (N == 1) {
          return SymbolicExecutor::SymbolicValue(
              SymbolicExecutor::SymbolicValue::Kind::Int, x, solver.make_true()
          );
        }
        // CONCAT bit_0, bit_1, ..., bit_{N-1} — bit_0 ends up in the high
        // position because CONCAT's first operand is high.
        auto result = solver.make_term(smt::Kind::BV_EXTRACT, {x}, {0, 0});
        for (uint32_t k = 1; k < N; ++k) {
          auto bit = solver.make_term(smt::Kind::BV_EXTRACT, {x}, {k, k});
          result = solver.make_term(smt::Kind::BV_CONCAT, {result, bit});
        }
        return SymbolicExecutor::SymbolicValue(
            SymbolicExecutor::SymbolicValue::Kind::Int, result, solver.make_true()
        );
      }
    };

    /**
     * @brief Common UB check + rotation construction for @rotl / @rotr.
     * @param leftRot true for rotate-left; false for rotate-right.
     */
    static smt::Term emitRotation(
        bool leftRot, uint32_t N, smt::Term x, smt::Term n, smt::Sort bvN, smt::ISolver &solver,
        std::vector<smt::Term> &pc
    ) {
      auto zero = solver.make_bv_value_int64(bvN, 0);
      auto nWide = solver.make_bv_value_int64(bvN, N);
      pc.push_back(solver.make_term(smt::Kind::BV_SGE, {n, zero}));
      pc.push_back(solver.make_term(smt::Kind::BV_SLT, {n, nWide}));
      // Use shift-and-or formulation. For n == 0, the inverse shift by N
      // would be out of range; guard with ITE.
      auto eq0 = solver.make_term(smt::Kind::EQUAL, {n, zero});
      auto leftPart = solver.make_term(leftRot ? smt::Kind::BV_SHL : smt::Kind::BV_SHR, {x, n});
      auto nInv = solver.make_term(smt::Kind::BV_SUB, {nWide, n});
      auto rightPart = solver.make_term(leftRot ? smt::Kind::BV_SHR : smt::Kind::BV_SHL, {x, nInv});
      auto combined = solver.make_term(smt::Kind::BV_OR, {leftPart, rightPart});
      return solver.make_term(smt::Kind::ITE, {eq0, x, combined});
    }

    class RotlIntrinsic final : public SolverIntrinsic {
    public:
      SymbolicExecutor::SymbolicValue solve(
          const IntrinsicDecl &, uint32_t N,
          const std::vector<SymbolicExecutor::SymbolicValue> &argVals, smt::Sort bvN,
          smt::ISolver &solver, std::vector<smt::Term> &pc
      ) const override {
        auto r = emitRotation(true, N, argVals[0].term, argVals[1].term, bvN, solver, pc);
        return SymbolicExecutor::SymbolicValue(
            SymbolicExecutor::SymbolicValue::Kind::Int, r, solver.make_true()
        );
      }
    };

    class RotrIntrinsic final : public SolverIntrinsic {
    public:
      SymbolicExecutor::SymbolicValue solve(
          const IntrinsicDecl &, uint32_t N,
          const std::vector<SymbolicExecutor::SymbolicValue> &argVals, smt::Sort bvN,
          smt::ISolver &solver, std::vector<smt::Term> &pc
      ) const override {
        auto r = emitRotation(false, N, argVals[0].term, argVals[1].term, bvN, solver, pc);
        return SymbolicExecutor::SymbolicValue(
            SymbolicExecutor::SymbolicValue::Kind::Int, r, solver.make_true()
        );
      }
    };

    class IsPow2Intrinsic final : public SolverIntrinsic {
    public:
      SymbolicExecutor::SymbolicValue solve(
          const IntrinsicDecl &intr, uint32_t,
          const std::vector<SymbolicExecutor::SymbolicValue> &argVals, smt::Sort,
          smt::ISolver &solver, std::vector<smt::Term> &
      ) const override {
        uint32_t pN = paramBitWidth(intr, 0);
        auto bvP = solver.make_bv_sort(pN);
        auto bv1 = solver.make_bv_sort(1);
        smt::Term x = argVals[0].term;
        auto zeroP = solver.make_bv_value_int64(bvP, 0);
        auto oneP = solver.make_bv_value_int64(bvP, 1);
        auto isPositive = solver.make_term(smt::Kind::BV_SGT, {x, zeroP});
        auto xMinus1 = solver.make_term(smt::Kind::BV_SUB, {x, oneP});
        auto andRes = solver.make_term(smt::Kind::BV_AND, {x, xMinus1});
        auto andIsZero = solver.make_term(smt::Kind::EQUAL, {andRes, zeroP});
        auto bothTrue = solver.make_term(smt::Kind::AND, {isPositive, andIsZero});
        auto one1 = solver.make_bv_value_int64(bv1, 1);
        auto zero1 = solver.make_bv_value_int64(bv1, 0);
        auto r = solver.make_term(smt::Kind::ITE, {bothTrue, one1, zero1});
        return SymbolicExecutor::SymbolicValue(
            SymbolicExecutor::SymbolicValue::Kind::Int, r, solver.make_true()
        );
      }
    };

    class Ilog2Intrinsic final : public SolverIntrinsic {
    public:
      SymbolicExecutor::SymbolicValue solve(
          const IntrinsicDecl &, uint32_t N,
          const std::vector<SymbolicExecutor::SymbolicValue> &argVals, smt::Sort bvN,
          smt::ISolver &solver, std::vector<smt::Term> &pc
      ) const override {
        smt::Term x = argVals[0].term;
        auto zero = solver.make_bv_value_int64(bvN, 0);
        // UB: x > 0 (signed).
        pc.push_back(solver.make_term(smt::Kind::BV_SGT, {x, zero}));

        // Build clz ITE chain, then return (N - 1) - clz. Because x > 0, the
        // sign bit is 0; the highest set bit is at most N-2, so result is in
        // [0, N-2].
        smt::Term clz = solver.make_bv_value_int64(bvN, N);
        auto one1 = solver.make_bv_value_int64(solver.make_bv_sort(1), 1);
        for (int i = (int) N - 1; i >= 0; --i) {
          int bitPos = (int) N - 1 - i;
          auto bit =
              solver.make_term(smt::Kind::BV_EXTRACT, {x}, {(uint32_t) bitPos, (uint32_t) bitPos});
          auto cond = solver.make_term(smt::Kind::EQUAL, {bit, one1});
          auto iT = solver.make_bv_value_int64(bvN, i);
          clz = solver.make_term(smt::Kind::ITE, {cond, iT, clz});
        }
        auto nMinus1 = solver.make_bv_value_int64(bvN, (int64_t) N - 1);
        auto r = solver.make_term(smt::Kind::BV_SUB, {nMinus1, clz});
        return SymbolicExecutor::SymbolicValue(
            SymbolicExecutor::SymbolicValue::Kind::Int, r, solver.make_true()
        );
      }
    };

    // ── Registry ────────────────────────────────────────────────────────────

    class IntrinsicRegistry {
    public:
      static const IntrinsicRegistry &get() {
        static IntrinsicRegistry instance;
        return instance;
      }

      const SolverIntrinsic *lookup(IntrinsicKind kind) const {
        auto it = registry_.find(kind);
        if (it != registry_.end())
          return it->second.get();
        return nullptr;
      }

    private:
      IntrinsicRegistry() {
        registry_[IntrinsicKind::Abs] = std::make_unique<AbsIntrinsic>();
        registry_[IntrinsicKind::Min] = std::make_unique<MinIntrinsic>();
        registry_[IntrinsicKind::Max] = std::make_unique<MaxIntrinsic>();
        registry_[IntrinsicKind::Popcount] = std::make_unique<PopcountIntrinsic>();
        registry_[IntrinsicKind::Clz] = std::make_unique<ClzIntrinsic>();
        registry_[IntrinsicKind::Ctz] = std::make_unique<CtzIntrinsic>();
        registry_[IntrinsicKind::AbsDiff] = std::make_unique<AbsDiffIntrinsic>();
        registry_[IntrinsicKind::Signum] = std::make_unique<SignumIntrinsic>();
        registry_[IntrinsicKind::Clamp] = std::make_unique<ClampIntrinsic>();
        registry_[IntrinsicKind::Midpoint] = std::make_unique<MidpointIntrinsic>();
        registry_[IntrinsicKind::Parity] = std::make_unique<ParityIntrinsic>();
        registry_[IntrinsicKind::Bswap] = std::make_unique<BswapIntrinsic>();
        registry_[IntrinsicKind::Bitreverse] = std::make_unique<BitreverseIntrinsic>();
        registry_[IntrinsicKind::Rotl] = std::make_unique<RotlIntrinsic>();
        registry_[IntrinsicKind::Rotr] = std::make_unique<RotrIntrinsic>();
        registry_[IntrinsicKind::IsPow2] = std::make_unique<IsPow2Intrinsic>();
        registry_[IntrinsicKind::Ilog2] = std::make_unique<Ilog2Intrinsic>();
      }

      std::unordered_map<IntrinsicKind, std::unique_ptr<SolverIntrinsic>> registry_;
    };

  } // namespace

  SymbolicExecutor::SymbolicValue SymbolicExecutor::callBuiltinIntrinsicSMT(
      const IntrinsicDecl &intr, std::vector<SymbolicValue> &argVals, smt::ISolver &solver,
      std::vector<smt::Term> &pc
  ) {
    uint32_t N = 32;
    if (auto pb = TypeUtils::getIntBitWidth(intr.retType))
      N = *pb;
    auto bvN = solver.make_bv_sort(N);

    auto kind = getIntrinsicKind(intr.name.name);
    if (kind) {
      if (auto impl = IntrinsicRegistry::get().lookup(*kind)) {
        return impl->solve(intr, N, argVals, bvN, solver, pc);
      }
    }

    throw std::runtime_error("Solver: unknown intrinsic " + intr.name.name);
  }

} // namespace symir
