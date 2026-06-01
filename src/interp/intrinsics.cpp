// [v0.2.2] Interpreter-side built-in intrinsic dispatch.
//
// This file is the single source of truth for every intrinsic
// supported by the SymIR interpreter. To add a new intrinsic:
//   1. Add its IntrinsicKind to include/analysis/intrinsics.hpp.
//   2. Implement a subclass of InterpreterIntrinsic and register it below.

#include "interp/interpreter.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include "analysis/intrinsics.hpp"
#include "analysis/type_utils.hpp"
#include "error.hpp"

namespace symir {

  namespace {

    /**
     * @brief Abstract base class for concrete evaluation of intrinsics.
     * Subclasses implement the concrete behavior of a specific intrinsic kind.
     */
    class InterpreterIntrinsic {
    public:
      virtual ~InterpreterIntrinsic() = default;

      /**
       * @brief Concrete evaluation of the intrinsic.
       * @param N Bit-width of the return type.
       * @param int_min_N The minimum signed value representable in N bits.
       * @param mask A bitmask representing N bits.
       * @param sext Helper function to sign-extend an int64_t from N bits to 64 bits.
       * @param intVal Helper function to safely read the i-th argument as a sign-extended int64_t.
       * @return The resulting RuntimeValue.
       */
      virtual Interpreter::RuntimeValue eval(
          uint32_t N, int64_t int_min_N, int64_t mask, const std::function<int64_t(int64_t)> &sext,
          const std::function<int64_t(size_t)> &intVal
      ) const = 0;
    };

    /**
     * @brief Implementation of the @abs(x) intrinsic.
     * Computes the signed absolute value of x.
     * Raises UB if x is the minimum representable value (INT_MIN_N).
     */
    class AbsIntrinsic final : public InterpreterIntrinsic {
    public:
      Interpreter::RuntimeValue eval(
          uint32_t N, int64_t int_min_N, int64_t /*mask*/,
          const std::function<int64_t(int64_t)> &sext, const std::function<int64_t(size_t)> &intVal
      ) const override {
        int64_t x = intVal(0);
        if (x == int_min_N)
          throw UndefinedBehaviorError("UB: @abs result not representable (-INT_MIN_N overflow)");
        Interpreter::RuntimeValue res;
        res.kind = Interpreter::RuntimeValue::Kind::Int;
        res.bits = N;
        res.intVal = sext(x < 0 ? -x : x);
        return res;
      }
    };

    /**
     * @brief Implementation of the @min(a, b) intrinsic.
     * Computes the signed minimum of a and b.
     */
    class MinIntrinsic final : public InterpreterIntrinsic {
    public:
      Interpreter::RuntimeValue eval(
          uint32_t N, int64_t /*int_min_N*/, int64_t /*mask*/,
          const std::function<int64_t(int64_t)> &sext, const std::function<int64_t(size_t)> &intVal
      ) const override {
        int64_t a = intVal(0), b = intVal(1);
        Interpreter::RuntimeValue res;
        res.kind = Interpreter::RuntimeValue::Kind::Int;
        res.bits = N;
        res.intVal = sext(a < b ? a : b);
        return res;
      }
    };

    /**
     * @brief Implementation of the @max(a, b) intrinsic.
     * Computes the signed maximum of a and b.
     */
    class MaxIntrinsic final : public InterpreterIntrinsic {
    public:
      Interpreter::RuntimeValue eval(
          uint32_t N, int64_t /*int_min_N*/, int64_t /*mask*/,
          const std::function<int64_t(int64_t)> &sext, const std::function<int64_t(size_t)> &intVal
      ) const override {
        int64_t a = intVal(0), b = intVal(1);
        Interpreter::RuntimeValue res;
        res.kind = Interpreter::RuntimeValue::Kind::Int;
        res.bits = N;
        res.intVal = sext(a > b ? a : b);
        return res;
      }
    };

    /**
     * @brief Implementation of the @popcount(x) intrinsic.
     * Counts the number of set bits (1s) in the value x.
     * Raises UB if the count exceeds the maximum signed value representable in N bits.
     */
    class PopcountIntrinsic final : public InterpreterIntrinsic {
    public:
      Interpreter::RuntimeValue eval(
          uint32_t N, int64_t /*int_min_N*/, int64_t mask,
          const std::function<int64_t(int64_t)> &sext, const std::function<int64_t(size_t)> &intVal
      ) const override {
        uint64_t u = static_cast<uint64_t>(intVal(0)) & static_cast<uint64_t>(mask);
        int64_t c = __builtin_popcountll(u);
        const int64_t signed_max = (N >= 64) ? INT64_MAX : ((INT64_C(1) << (N - 1)) - 1);
        if (c > signed_max)
          throw UndefinedBehaviorError("UB: @popcount result not representable in declared iN");
        Interpreter::RuntimeValue res;
        res.kind = Interpreter::RuntimeValue::Kind::Int;
        res.bits = N;
        res.intVal = sext(c);
        return res;
      }
    };

    /**
     * @brief Implementation of the @clz(x) intrinsic.
     * Counts the number of leading zero bits in x from the N-1-th bit down to 0.
     * Raises UB if input x is 0.
     */
    class ClzIntrinsic final : public InterpreterIntrinsic {
    public:
      Interpreter::RuntimeValue eval(
          uint32_t N, int64_t /*int_min_N*/, int64_t mask,
          const std::function<int64_t(int64_t)> &sext, const std::function<int64_t(size_t)> &intVal
      ) const override {
        uint64_t u = static_cast<uint64_t>(intVal(0)) & static_cast<uint64_t>(mask);
        if (u == 0)
          throw UndefinedBehaviorError("UB: @clz requires non-zero input (§12.2)");
        int64_t c = 0;
        for (int b = (int) N - 1; b >= 0; --b) {
          if ((u >> b) & 1ULL)
            break;
          ++c;
        }
        Interpreter::RuntimeValue res;
        res.kind = Interpreter::RuntimeValue::Kind::Int;
        res.bits = N;
        res.intVal = sext(c);
        return res;
      }
    };

    /**
     * @brief Implementation of the @ctz(x) intrinsic.
     * Counts the number of trailing zero bits in x from the 0-th bit up to N-1.
     * Raises UB if input x is 0.
     */
    class CtzIntrinsic final : public InterpreterIntrinsic {
    public:
      Interpreter::RuntimeValue eval(
          uint32_t N, int64_t /*int_min_N*/, int64_t mask,
          const std::function<int64_t(int64_t)> &sext, const std::function<int64_t(size_t)> &intVal
      ) const override {
        uint64_t u = static_cast<uint64_t>(intVal(0)) & static_cast<uint64_t>(mask);
        if (u == 0)
          throw UndefinedBehaviorError("UB: @ctz requires non-zero input (§12.2)");
        int64_t c = 0;
        for (uint32_t b = 0; b < N; ++b) {
          if ((u >> b) & 1ULL)
            break;
          ++c;
        }
        Interpreter::RuntimeValue res;
        res.kind = Interpreter::RuntimeValue::Kind::Int;
        res.bits = N;
        res.intVal = sext(c);
        return res;
      }
    };

    /**
     * @brief Centralized registry of all interpreter intrinsic implementations.
     * Maps IntrinsicKind to its corresponding class handler.
     */
    class IntrinsicRegistry {
    public:
      static const IntrinsicRegistry &get() {
        static IntrinsicRegistry instance;
        return instance;
      }

      const InterpreterIntrinsic *lookup(IntrinsicKind kind) const {
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

      std::unordered_map<IntrinsicKind, std::unique_ptr<InterpreterIntrinsic>> registry_;
    };

  } // namespace

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
    auto sext = [N, mask](int64_t v) -> int64_t {
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

    auto kind = getIntrinsicKind(intr.name.name);
    if (kind) {
      if (auto impl = IntrinsicRegistry::get().lookup(*kind)) {
        return impl->eval(N, int_min_N, mask, sext, intVal);
      }
    }

    throw std::runtime_error("Unknown intrinsic: " + intr.name.name);
  }

} // namespace symir
