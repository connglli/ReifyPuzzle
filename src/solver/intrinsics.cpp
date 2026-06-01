// [v0.2.2] Solver-side SMT lowering for built-in intrinsics.
//
// This file is the single source of truth for every intrinsic
// supported by the SymIR symbolic executor / SMT constraint generator.
// To add a new intrinsic:
//   1. Add its IntrinsicKind to include/analysis/intrinsics.hpp.
//   2. Implement a subclass of SolverIntrinsic and register it below.

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
     * @brief Abstract base class for lowering built-in intrinsics to SMT terms.
     * Subclasses implement the translation logic using SMT bit-vector logic.
     */
    class SolverIntrinsic {
    public:
      virtual ~SolverIntrinsic() = default;

      /**
       * @brief Lowers the intrinsic call into a SymbolicValue.
       * @param N Declared bit-width of the intrinsic.
       * @param argVals Symbolic values for all inputs.
       * @param bvN Bit-vector sort of width N.
       * @param solver Reference to the SMT solver backend.
       * @param pc Accumulator for path conditions / UB constraints.
       * @return The lowered symbolic result.
       */
      virtual SymbolicExecutor::SymbolicValue solve(
          uint32_t N, const std::vector<SymbolicExecutor::SymbolicValue> &argVals, smt::Sort bvN,
          smt::ISolver &solver, std::vector<smt::Term> &pc
      ) const = 0;
    };

    /**
     * @brief SMT translation for the @abs(x) intrinsic.
     * Encodes absolute value using an SMT ITE term: (ITE (x >= 0) x (- x)).
     * Appends an SMT DISTINCT assertion to check for UB (x != INT_MIN_N).
     */
    class AbsIntrinsic final : public SolverIntrinsic {
    public:
      SymbolicExecutor::SymbolicValue solve(
          uint32_t N, const std::vector<SymbolicExecutor::SymbolicValue> &argVals, smt::Sort bvN,
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

    /**
     * @brief SMT translation for the @min(a, b) intrinsic.
     * Encodes minimum using (ITE (a <= b) a b) with signed comparison (BV_SLE).
     */
    class MinIntrinsic final : public SolverIntrinsic {
    public:
      SymbolicExecutor::SymbolicValue solve(
          uint32_t /*N*/, const std::vector<SymbolicExecutor::SymbolicValue> &argVals,
          smt::Sort /*bvN*/, smt::ISolver &solver, std::vector<smt::Term> & /*pc*/
      ) const override {
        smt::Term a0 = argVals[0].term;
        smt::Term a1 = argVals[1].term;
        auto cond = solver.make_term(smt::Kind::BV_SLE, {a0, a1});
        auto res = solver.make_term(smt::Kind::ITE, {cond, a0, a1});
        return SymbolicExecutor::SymbolicValue(
            SymbolicExecutor::SymbolicValue::Kind::Int, res, solver.make_true()
        );
      }
    };

    /**
     * @brief SMT translation for the @max(a, b) intrinsic.
     * Encodes maximum using (ITE (a >= b) a b) with signed comparison (BV_SGE).
     */
    class MaxIntrinsic final : public SolverIntrinsic {
    public:
      SymbolicExecutor::SymbolicValue solve(
          uint32_t /*N*/, const std::vector<SymbolicExecutor::SymbolicValue> &argVals,
          smt::Sort /*bvN*/, smt::ISolver &solver, std::vector<smt::Term> & /*pc*/
      ) const override {
        smt::Term a0 = argVals[0].term;
        smt::Term a1 = argVals[1].term;
        auto cond = solver.make_term(smt::Kind::BV_SGE, {a0, a1});
        auto res = solver.make_term(smt::Kind::ITE, {cond, a0, a1});
        return SymbolicExecutor::SymbolicValue(
            SymbolicExecutor::SymbolicValue::Kind::Int, res, solver.make_true()
        );
      }
    };

    /**
     * @brief SMT translation for the @popcount(x) intrinsic.
     * Encodes population count as the sum of all bits extracted from x.
     * Extracts each bit as a 1-bit value, zero-extends it to width N,
     * and accumulates using bit-vector addition (BV_ADD).
     */
    class PopcountIntrinsic final : public SolverIntrinsic {
    public:
      SymbolicExecutor::SymbolicValue solve(
          uint32_t N, const std::vector<SymbolicExecutor::SymbolicValue> &argVals, smt::Sort bvN,
          smt::ISolver &solver, std::vector<smt::Term> & /*pc*/
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

    /**
     * @brief SMT translation for the @clz(x) intrinsic.
     * Encodes leading zero count by building a cascading chain of SMT ITEs.
     * Traverses bit positions from most significant to least significant.
     * Appends a DISTINCT assertion to require input x != 0.
     */
    class ClzIntrinsic final : public SolverIntrinsic {
    public:
      SymbolicExecutor::SymbolicValue solve(
          uint32_t N, const std::vector<SymbolicExecutor::SymbolicValue> &argVals, smt::Sort bvN,
          smt::ISolver &solver, std::vector<smt::Term> &pc
      ) const override {
        smt::Term x = argVals[0].term;
        auto zero = solver.make_bv_value_int64(bvN, 0);
        pc.push_back(solver.make_term(smt::Kind::DISTINCT, {x, zero}));

        smt::Term defaultT = solver.make_bv_value_int64(bvN, N);
        smt::Term result = defaultT;
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

    /**
     * @brief SMT translation for the @ctz(x) intrinsic.
     * Encodes trailing zero count by building a cascading chain of SMT ITEs.
     * Traverses bit positions from least significant to most significant.
     * Appends a DISTINCT assertion to require input x != 0.
     */
    class CtzIntrinsic final : public SolverIntrinsic {
    public:
      SymbolicExecutor::SymbolicValue solve(
          uint32_t N, const std::vector<SymbolicExecutor::SymbolicValue> &argVals, smt::Sort bvN,
          smt::ISolver &solver, std::vector<smt::Term> &pc
      ) const override {
        smt::Term x = argVals[0].term;
        auto zero = solver.make_bv_value_int64(bvN, 0);
        pc.push_back(solver.make_term(smt::Kind::DISTINCT, {x, zero}));

        smt::Term defaultT = solver.make_bv_value_int64(bvN, N);
        smt::Term result = defaultT;
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

    /**
     * @brief Registry class to instantiate and find solver intrinsics.
     */
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
        return impl->solve(N, argVals, bvN, solver, pc);
      }
    }

    throw std::runtime_error("Solver: unknown intrinsic " + intr.name.name);
  }

} // namespace symir
