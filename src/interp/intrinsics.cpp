// [v0.2.2] Interpreter-side built-in intrinsic dispatch.
//
// This file is the single source of truth for every intrinsic
// supported by the SymIR interpreter. To add a new intrinsic:
//   1. Add its IntrinsicKind to include/analysis/intrinsics.hpp.
//   2. Implement a subclass of InterpreterIntrinsic and register it below.
//
// Width conventions:
//   - `N` is the return-type bit-width (read from intr.retType).
//   - For predicate intrinsics (@parity, @is_pow2) the return width is
//     fixed at 1 (i1) while the input width is iN. The eval API exposes
//     `intr` so impls can read per-parameter widths from the declaration.
//   - `args[i].intVal` is already sign-extended to int64 from its
//     declared width by the caller, so reading raw arg bits requires
//     re-masking against the parameter's own width — not the return's.

#include "interp/interpreter.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
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
     * @brief Signed-bit mask for an N-bit slice of a 64-bit value.
     */
    inline int64_t maskOfN(uint32_t N) { return (N >= 64) ? -1LL : ((INT64_C(1) << N) - 1); }

    /**
     * @brief Minimum signed value representable in N bits.
     */
    inline int64_t intMinN(uint32_t N) { return (N >= 64) ? INT64_MIN : -(INT64_C(1) << (N - 1)); }

    /**
     * @brief Maximum signed value representable in N bits.
     */
    inline int64_t intMaxN(uint32_t N) {
      return (N >= 64) ? INT64_MAX : ((INT64_C(1) << (N - 1)) - 1);
    }

    /**
     * @brief Sign-extend a 64-bit value as if its low N bits encode an
     * N-bit signed integer.
     */
    inline int64_t sextToInt64(int64_t v, uint32_t N) {
      if (N >= 64)
        return v;
      int64_t m = maskOfN(N);
      v &= m;
      if (v & (INT64_C(1) << (N - 1)))
        v |= ~m;
      return v;
    }

    /**
     * @brief Unsigned interpretation of an N-bit slice of a 64-bit value.
     */
    inline uint64_t uintOfN(int64_t v, uint32_t N) {
      return static_cast<uint64_t>(v) & static_cast<uint64_t>(maskOfN(N));
    }

    /**
     * @brief Read the i-th argument as a 64-bit signed value, sign-extended
     * from the *parameter's own width* (NOT the return width).
     */
    inline int64_t argSint(
        const IntrinsicDecl &intr, const std::vector<Interpreter::RuntimeValue> &args, size_t i
    ) {
      if (i >= args.size())
        throw std::runtime_error("Intrinsic " + intr.name.name + ": argument count error");
      if (args[i].kind != Interpreter::RuntimeValue::Kind::Int)
        throw std::runtime_error("Intrinsic " + intr.name.name + ": non-integer argument");
      auto pb = TypeUtils::getIntBitWidth(intr.params[i].type);
      uint32_t pN = pb ? *pb : args[i].bits;
      // [v0.2.2] i1 (boolean) uses unsigned convention (0/1), not signed (-1/0).
      if (pN == 1)
        return args[i].intVal & 1;
      return sextToInt64(args[i].intVal, pN);
    }

    /**
     * @brief Read the i-th argument as a 64-bit unsigned value, masked to
     * the parameter's own width.
     */
    inline uint64_t argUint(
        const IntrinsicDecl &intr, const std::vector<Interpreter::RuntimeValue> &args, size_t i
    ) {
      auto pb = TypeUtils::getIntBitWidth(intr.params[i].type);
      uint32_t pN = pb ? *pb : args[i].bits;
      return uintOfN(argSint(intr, args, i), pN);
    }

    /**
     * @brief Construct an integer-kind RuntimeValue with the given N-bit value.
     * For N >= 2, the value is sign-extended to int64 (the iN storage
     * convention). For N == 1 (i1 / predicate), the value is stored as
     * literal 0 or 1, matching the `cmp` instruction's convention in the
     * interpreter (see src/interp/interpreter.cpp:1609).
     */
    inline Interpreter::RuntimeValue makeInt(uint32_t N, int64_t v) {
      Interpreter::RuntimeValue r;
      r.kind = Interpreter::RuntimeValue::Kind::Int;
      r.bits = N;
      if (N == 1) {
        r.intVal = v & 1;
      } else {
        r.intVal = sextToInt64(v, N);
      }
      return r;
    }

    // ── v0.2.2 extra batch D.1 — floating-point helpers ─────────────────

    /**
     * @brief Width (32 or 64) of an FP parameter or return type.
     */
    inline uint32_t fpBitsOf(const TypePtr &t) {
      auto fp = std::get_if<FloatType>(&t->v);
      return (fp && fp->kind == FloatType::Kind::F32) ? 32 : 64;
    }

    /**
     * @brief Read the i-th argument as a double; for f32 parameters, the
     * value held in `floatVal` is already at f32 precision (see the
     * argument-binding path in interpreter.cpp).
     */
    inline double argFloat(
        const IntrinsicDecl & /*intr*/, const std::vector<Interpreter::RuntimeValue> &args, size_t i
    ) {
      return args[i].floatVal;
    }

    /**
     * @brief Construct a float-kind RuntimeValue. For N == 32 the value is
     * narrowed to f32 precision so subsequent reads agree with the C
     * backend's `float`-typed storage.
     */
    inline Interpreter::RuntimeValue makeFloat(uint32_t N, double v) {
      Interpreter::RuntimeValue r;
      r.kind = Interpreter::RuntimeValue::Kind::Float;
      r.bits = N;
      r.floatVal = (N == 32) ? static_cast<double>(static_cast<float>(v)) : v;
      return r;
    }

    /**
     * @brief Abstract base class for concrete evaluation of intrinsics.
     */
    class InterpreterIntrinsic {
    public:
      virtual ~InterpreterIntrinsic() = default;

      /**
       * @brief Concrete evaluation of the intrinsic.
       * @param intr Intrinsic declaration (for per-parameter width queries).
       * @param args Arguments, in declaration order.
       * @return The resulting RuntimeValue.
       */
      virtual Interpreter::RuntimeValue
      eval(const IntrinsicDecl &intr, const std::vector<Interpreter::RuntimeValue> &args) const = 0;
    };

    // ── §12.1 Arithmetic intrinsics ─────────────────────────────────────────

    class AbsIntrinsic final : public InterpreterIntrinsic {
    public:
      Interpreter::RuntimeValue eval(
          const IntrinsicDecl &intr, const std::vector<Interpreter::RuntimeValue> &args
      ) const override {
        uint32_t N = *TypeUtils::getIntBitWidth(intr.retType);
        int64_t x = argSint(intr, args, 0);
        if (x == intMinN(N))
          throw UndefinedBehaviorError("UB: @abs result not representable (-INT_MIN_N overflow)");
        return makeInt(N, x < 0 ? -x : x);
      }
    };

    class MinIntrinsic final : public InterpreterIntrinsic {
    public:
      Interpreter::RuntimeValue eval(
          const IntrinsicDecl &intr, const std::vector<Interpreter::RuntimeValue> &args
      ) const override {
        uint32_t N = *TypeUtils::getIntBitWidth(intr.retType);
        int64_t a = argSint(intr, args, 0), b = argSint(intr, args, 1);
        return makeInt(N, a < b ? a : b);
      }
    };

    class MaxIntrinsic final : public InterpreterIntrinsic {
    public:
      Interpreter::RuntimeValue eval(
          const IntrinsicDecl &intr, const std::vector<Interpreter::RuntimeValue> &args
      ) const override {
        uint32_t N = *TypeUtils::getIntBitWidth(intr.retType);
        int64_t a = argSint(intr, args, 0), b = argSint(intr, args, 1);
        return makeInt(N, a > b ? a : b);
      }
    };

    // ── §12.2 Bit-counting intrinsics ───────────────────────────────────────

    class PopcountIntrinsic final : public InterpreterIntrinsic {
    public:
      Interpreter::RuntimeValue eval(
          const IntrinsicDecl &intr, const std::vector<Interpreter::RuntimeValue> &args
      ) const override {
        uint32_t N = *TypeUtils::getIntBitWidth(intr.retType);
        uint64_t u = argUint(intr, args, 0);
        int64_t c = __builtin_popcountll(u);
        if (c > intMaxN(N))
          throw UndefinedBehaviorError("UB: @popcount result not representable in declared iN");
        return makeInt(N, c);
      }
    };

    class ClzIntrinsic final : public InterpreterIntrinsic {
    public:
      Interpreter::RuntimeValue eval(
          const IntrinsicDecl &intr, const std::vector<Interpreter::RuntimeValue> &args
      ) const override {
        uint32_t N = *TypeUtils::getIntBitWidth(intr.retType);
        uint64_t u = argUint(intr, args, 0);
        if (u == 0)
          throw UndefinedBehaviorError("UB: @clz requires non-zero input (§12.2)");
        int64_t c = 0;
        for (int b = (int) N - 1; b >= 0; --b) {
          if ((u >> b) & 1ULL)
            break;
          ++c;
        }
        return makeInt(N, c);
      }
    };

    class CtzIntrinsic final : public InterpreterIntrinsic {
    public:
      Interpreter::RuntimeValue eval(
          const IntrinsicDecl &intr, const std::vector<Interpreter::RuntimeValue> &args
      ) const override {
        uint32_t N = *TypeUtils::getIntBitWidth(intr.retType);
        uint64_t u = argUint(intr, args, 0);
        if (u == 0)
          throw UndefinedBehaviorError("UB: @ctz requires non-zero input (§12.2)");
        int64_t c = 0;
        for (uint32_t b = 0; b < N; ++b) {
          if ((u >> b) & 1ULL)
            break;
          ++c;
        }
        return makeInt(N, c);
      }
    };

    // ── §12.3 Integer extras (v0.2.2 extra batch A) ────────────────────────────────

    /**
     * @brief @abs_diff(a, b) = |a - b|, UB if the result is not representable in iN.
     * Computed via signed widening to int64 (or __int128 for N == 64) — no
     * reinterpretation as unsigned values is performed.
     */
    class AbsDiffIntrinsic final : public InterpreterIntrinsic {
    public:
      Interpreter::RuntimeValue eval(
          const IntrinsicDecl &intr, const std::vector<Interpreter::RuntimeValue> &args
      ) const override {
        uint32_t N = *TypeUtils::getIntBitWidth(intr.retType);
        int64_t a = argSint(intr, args, 0), b = argSint(intr, args, 1);
        int64_t maxN = intMaxN(N);
        if (N < 64) {
          int64_t s = a - b;
          int64_t r = s < 0 ? -s : s;
          if (r > maxN)
            throw UndefinedBehaviorError(
                "UB: @abs_diff result not representable in declared iN (|a-b| > INT_MAX_N)"
            );
          return makeInt(N, r);
        }
#if defined(__SIZEOF_INT128__)
        __int128 s = static_cast<__int128>(a) - static_cast<__int128>(b);
        __int128 r = s < 0 ? -s : s;
        if (r > static_cast<__int128>(INT64_MAX))
          throw UndefinedBehaviorError("UB: @abs_diff result not representable in declared i64");
        return makeInt(N, static_cast<int64_t>(r));
#else
        // Portable fallback for hosts lacking __int128. Detect overflow by
        // sign analysis: a - b overflows when a and b have different signs
        // and the result has a sign opposite to a.
        bool diffSigns = ((a < 0) != (b < 0));
        int64_t s = a - b;
        if (diffSigns && ((a < 0) != (s < 0)))
          throw UndefinedBehaviorError("UB: @abs_diff result not representable in declared i64");
        int64_t r = s < 0 ? -s : s;
        if (s == INT64_MIN)
          throw UndefinedBehaviorError("UB: @abs_diff result not representable in declared i64");
        return makeInt(N, r);
#endif
      }
    };

    /**
     * @brief @signum(x) returns sign of x in {-1, 0, 1}.
     */
    class SignumIntrinsic final : public InterpreterIntrinsic {
    public:
      Interpreter::RuntimeValue eval(
          const IntrinsicDecl &intr, const std::vector<Interpreter::RuntimeValue> &args
      ) const override {
        uint32_t N = *TypeUtils::getIntBitWidth(intr.retType);
        int64_t x = argSint(intr, args, 0);
        int64_t r = (x > 0) - (x < 0);
        return makeInt(N, r);
      }
    };

    /**
     * @brief @clamp(v, lo, hi) — signed clamp; UB if lo > hi.
     */
    class ClampIntrinsic final : public InterpreterIntrinsic {
    public:
      Interpreter::RuntimeValue eval(
          const IntrinsicDecl &intr, const std::vector<Interpreter::RuntimeValue> &args
      ) const override {
        uint32_t N = *TypeUtils::getIntBitWidth(intr.retType);
        int64_t v = argSint(intr, args, 0);
        int64_t lo = argSint(intr, args, 1);
        int64_t hi = argSint(intr, args, 2);
        if (lo > hi)
          throw UndefinedBehaviorError("UB: @clamp requires lo <= hi");
        int64_t r = v < lo ? lo : (v > hi ? hi : v);
        return makeInt(N, r);
      }
    };

    /**
     * @brief @midpoint(a, b) — (a + b) / 2, truncating toward zero.
     */
    class MidpointIntrinsic final : public InterpreterIntrinsic {
    public:
      Interpreter::RuntimeValue eval(
          const IntrinsicDecl &intr, const std::vector<Interpreter::RuntimeValue> &args
      ) const override {
        uint32_t N = *TypeUtils::getIntBitWidth(intr.retType);
        int64_t a = argSint(intr, args, 0), b = argSint(intr, args, 1);
        // For N <= 63, (a + b) always fits in int64. For N == 64, do the
        // sign-extend-by-1 trick via __int128 (or split). Use __int128 when
        // available; fall back to a portable computation otherwise.
        if (N <= 63) {
          int64_t s = a + b;
          int64_t r = s / 2; // truncation toward zero (C/C++ since C99)
          return makeInt(N, r);
        }
        // N == 64: compute via 128-bit if available.
#if defined(__SIZEOF_INT128__)
        __int128 s = static_cast<__int128>(a) + static_cast<__int128>(b);
        __int128 r = s / 2;
        return makeInt(N, static_cast<int64_t>(r));
#else
        // Portable fallback: ((a & b) + ((a ^ b) >> 1)) gives floor((a+b)/2);
        // adjust by +1 when the sum is negative-but-not-divisible to round
        // toward zero instead of toward -inf.
        int64_t floor_avg = (a & b) + ((a ^ b) >> 1);
        int64_t r = floor_avg;
        if ((a ^ b) & 1 && floor_avg < 0)
          r += 1;
        return makeInt(N, r);
#endif
      }
    };

    // ── §12.4 Bit-manipulation (v0.2.2 extra batch B) ──────────────────────────────

    /**
     * @brief @parity(x) -> i1; XOR of all bits.
     */
    class ParityIntrinsic final : public InterpreterIntrinsic {
    public:
      Interpreter::RuntimeValue eval(
          const IntrinsicDecl &intr, const std::vector<Interpreter::RuntimeValue> &args
      ) const override {
        uint64_t u = argUint(intr, args, 0);
        int64_t p = __builtin_parityll(u);
        return makeInt(1, p & 1);
      }
    };

    /**
     * @brief @bswap(x) — reverse byte order. Declaration is rejected if N % 8 != 0.
     */
    class BswapIntrinsic final : public InterpreterIntrinsic {
    public:
      Interpreter::RuntimeValue eval(
          const IntrinsicDecl &intr, const std::vector<Interpreter::RuntimeValue> &args
      ) const override {
        uint32_t N = *TypeUtils::getIntBitWidth(intr.retType);
        uint64_t u = argUint(intr, args, 0);
        uint32_t nbytes = N / 8;
        uint64_t r = 0;
        for (uint32_t i = 0; i < nbytes; ++i) {
          uint64_t byte = (u >> (i * 8)) & 0xFFULL;
          r |= byte << ((nbytes - 1 - i) * 8);
        }
        return makeInt(N, static_cast<int64_t>(r));
      }
    };

    /**
     * @brief @bitreverse(x) — reverse all N bits.
     */
    class BitreverseIntrinsic final : public InterpreterIntrinsic {
    public:
      Interpreter::RuntimeValue eval(
          const IntrinsicDecl &intr, const std::vector<Interpreter::RuntimeValue> &args
      ) const override {
        uint32_t N = *TypeUtils::getIntBitWidth(intr.retType);
        uint64_t u = argUint(intr, args, 0);
        uint64_t r = 0;
        for (uint32_t i = 0; i < N; ++i) {
          if ((u >> i) & 1ULL)
            r |= UINT64_C(1) << (N - 1 - i);
        }
        return makeInt(N, static_cast<int64_t>(r));
      }
    };

    /**
     * @brief @rotl(x, n) — rotate x left by n positions. UB if n < 0 or n >= N.
     */
    class RotlIntrinsic final : public InterpreterIntrinsic {
    public:
      Interpreter::RuntimeValue eval(
          const IntrinsicDecl &intr, const std::vector<Interpreter::RuntimeValue> &args
      ) const override {
        uint32_t N = *TypeUtils::getIntBitWidth(intr.retType);
        uint64_t u = argUint(intr, args, 0);
        int64_t n = argSint(intr, args, 1);
        if (n < 0 || n >= static_cast<int64_t>(N))
          throw UndefinedBehaviorError("UB: @rotl requires 0 <= n < N");
        uint64_t r;
        if (n == 0)
          r = u;
        else
          r = ((u << n) | (u >> (N - n))) & static_cast<uint64_t>(maskOfN(N));
        return makeInt(N, static_cast<int64_t>(r));
      }
    };

    /**
     * @brief @rotr(x, n) — rotate x right by n positions. UB if n < 0 or n >= N.
     */
    class RotrIntrinsic final : public InterpreterIntrinsic {
    public:
      Interpreter::RuntimeValue eval(
          const IntrinsicDecl &intr, const std::vector<Interpreter::RuntimeValue> &args
      ) const override {
        uint32_t N = *TypeUtils::getIntBitWidth(intr.retType);
        uint64_t u = argUint(intr, args, 0);
        int64_t n = argSint(intr, args, 1);
        if (n < 0 || n >= static_cast<int64_t>(N))
          throw UndefinedBehaviorError("UB: @rotr requires 0 <= n < N");
        uint64_t r;
        if (n == 0)
          r = u;
        else
          r = ((u >> n) | (u << (N - n))) & static_cast<uint64_t>(maskOfN(N));
        return makeInt(N, static_cast<int64_t>(r));
      }
    };

    /**
     * @brief @is_pow2(x) -> i1; true iff x is a positive power of two.
     */
    class IsPow2Intrinsic final : public InterpreterIntrinsic {
    public:
      Interpreter::RuntimeValue eval(
          const IntrinsicDecl &intr, const std::vector<Interpreter::RuntimeValue> &args
      ) const override {
        int64_t x = argSint(intr, args, 0);
        uint64_t u = static_cast<uint64_t>(x);
        int64_t r = (x > 0 && (u & (u - 1)) == 0) ? 1 : 0;
        return makeInt(1, r);
      }
    };

    /**
     * @brief @ilog2(x) — floor(log2(x)); UB if x <= 0.
     */
    class Ilog2Intrinsic final : public InterpreterIntrinsic {
    public:
      Interpreter::RuntimeValue eval(
          const IntrinsicDecl &intr, const std::vector<Interpreter::RuntimeValue> &args
      ) const override {
        uint32_t N = *TypeUtils::getIntBitWidth(intr.retType);
        int64_t x = argSint(intr, args, 0);
        if (x <= 0)
          throw UndefinedBehaviorError("UB: @ilog2 requires x > 0");
        uint64_t u = static_cast<uint64_t>(x);
        int64_t r = 63 - __builtin_clzll(u);
        return makeInt(N, r);
      }
    };

    // ── §12.5 Integer overflow-aware family (v0.2.2 extra batch C) ────────
    //
    // Conventions: all members are declared at one common iN.  Width-mod
    // arithmetic is computed on int64 and then masked / sign-extended
    // back to iN via makeInt.  No reinterpretation as unsigned —
    // saturation bounds and div_euclid signs are all derived from the
    // signed semantics of iN.

    /**
     * @brief @wrapping_add(a, b) — `(a + b) mod 2^N`, sign-extended to iN.
     */
    class WrappingAddIntrinsic final : public InterpreterIntrinsic {
    public:
      Interpreter::RuntimeValue eval(
          const IntrinsicDecl &intr, const std::vector<Interpreter::RuntimeValue> &args
      ) const override {
        uint32_t N = *TypeUtils::getIntBitWidth(intr.retType);
        uint64_t a = argUint(intr, args, 0), b = argUint(intr, args, 1);
        return makeInt(N, static_cast<int64_t>(a + b));
      }
    };

    /**
     * @brief @wrapping_sub(a, b) — `(a − b) mod 2^N`, sign-extended to iN.
     */
    class WrappingSubIntrinsic final : public InterpreterIntrinsic {
    public:
      Interpreter::RuntimeValue eval(
          const IntrinsicDecl &intr, const std::vector<Interpreter::RuntimeValue> &args
      ) const override {
        uint32_t N = *TypeUtils::getIntBitWidth(intr.retType);
        uint64_t a = argUint(intr, args, 0), b = argUint(intr, args, 1);
        return makeInt(N, static_cast<int64_t>(a - b));
      }
    };

    /**
     * @brief @wrapping_mul(a, b) — `(a * b) mod 2^N`, sign-extended to iN.
     */
    class WrappingMulIntrinsic final : public InterpreterIntrinsic {
    public:
      Interpreter::RuntimeValue eval(
          const IntrinsicDecl &intr, const std::vector<Interpreter::RuntimeValue> &args
      ) const override {
        uint32_t N = *TypeUtils::getIntBitWidth(intr.retType);
        uint64_t a = argUint(intr, args, 0), b = argUint(intr, args, 1);
        // Unsigned mul on the iN-sized values; for N < 64 the high bits
        // are discarded by makeInt's mask + sign-extend.  For N == 64 the
        // low 64 bits of a 128-bit product equal `(uint64_t)(a * b)`.
        return makeInt(N, static_cast<int64_t>(a * b));
      }
    };

    /**
     * @brief @wrapping_neg(x) — `(-x) mod 2^N`.  Maps INT_MIN_N to itself.
     */
    class WrappingNegIntrinsic final : public InterpreterIntrinsic {
    public:
      Interpreter::RuntimeValue eval(
          const IntrinsicDecl &intr, const std::vector<Interpreter::RuntimeValue> &args
      ) const override {
        uint32_t N = *TypeUtils::getIntBitWidth(intr.retType);
        uint64_t a = argUint(intr, args, 0);
        return makeInt(N, static_cast<int64_t>(-a));
      }
    };

    /**
     * @brief @wrapping_shl(x, n) — left-shift modulo 2^N.  UB if `n < 0` or
     * `n >= N` (same as the OpAtom shift rule §7.1 rule 5).
     */
    class WrappingShlIntrinsic final : public InterpreterIntrinsic {
    public:
      Interpreter::RuntimeValue eval(
          const IntrinsicDecl &intr, const std::vector<Interpreter::RuntimeValue> &args
      ) const override {
        uint32_t N = *TypeUtils::getIntBitWidth(intr.retType);
        uint64_t a = argUint(intr, args, 0);
        int64_t n = argSint(intr, args, 1);
        if (n < 0 || n >= static_cast<int64_t>(N))
          throw UndefinedBehaviorError("UB: @wrapping_shl requires 0 <= n < N");
        return makeInt(N, static_cast<int64_t>(a << n));
      }
    };

    /**
     * @brief @wrapping_shr(x, n) — arithmetic right-shift mod 2^N.  UB if
     * `n < 0` or `n >= N`.  Sign bit is preserved (matches SymIR's signed
     * convention; `LShr` is the OpAtom path for logical shift).
     */
    class WrappingShrIntrinsic final : public InterpreterIntrinsic {
    public:
      Interpreter::RuntimeValue eval(
          const IntrinsicDecl &intr, const std::vector<Interpreter::RuntimeValue> &args
      ) const override {
        uint32_t N = *TypeUtils::getIntBitWidth(intr.retType);
        int64_t x = argSint(intr, args, 0);
        int64_t n = argSint(intr, args, 1);
        if (n < 0 || n >= static_cast<int64_t>(N))
          throw UndefinedBehaviorError("UB: @wrapping_shr requires 0 <= n < N");
        // Arithmetic shift in C++20 is implementation-defined before
        // C++20 but well-defined as sign-preserving since.  GCC / clang
        // both perform an arithmetic shift on signed types.
        return makeInt(N, x >> n);
      }
    };

    /**
     * @brief @saturating_add(a, b) — clamp the true sum to [INT_MIN_N,
     * INT_MAX_N].
     */
    class SaturatingAddIntrinsic final : public InterpreterIntrinsic {
    public:
      Interpreter::RuntimeValue eval(
          const IntrinsicDecl &intr, const std::vector<Interpreter::RuntimeValue> &args
      ) const override {
        uint32_t N = *TypeUtils::getIntBitWidth(intr.retType);
        int64_t a = argSint(intr, args, 0), b = argSint(intr, args, 1);
        int64_t lo = intMinN(N), hi = intMaxN(N);
#if defined(__SIZEOF_INT128__)
        __int128 s = static_cast<__int128>(a) + static_cast<__int128>(b);
        int64_t r = (s < lo) ? lo : (s > hi ? hi : static_cast<int64_t>(s));
#else
        // Fallback: detect overflow by sign analysis on the true sum.
        int64_t s = a + b;
        bool ovPos = (a > 0 && b > 0 && s < 0);
        bool ovNeg = (a < 0 && b < 0 && s >= 0);
        int64_t r = ovPos ? hi : ovNeg ? lo : (s < lo ? lo : (s > hi ? hi : s));
#endif
        return makeInt(N, r);
      }
    };

    /**
     * @brief @saturating_sub(a, b) — clamp the true difference to
     * [INT_MIN_N, INT_MAX_N].
     */
    class SaturatingSubIntrinsic final : public InterpreterIntrinsic {
    public:
      Interpreter::RuntimeValue eval(
          const IntrinsicDecl &intr, const std::vector<Interpreter::RuntimeValue> &args
      ) const override {
        uint32_t N = *TypeUtils::getIntBitWidth(intr.retType);
        int64_t a = argSint(intr, args, 0), b = argSint(intr, args, 1);
        int64_t lo = intMinN(N), hi = intMaxN(N);
#if defined(__SIZEOF_INT128__)
        __int128 s = static_cast<__int128>(a) - static_cast<__int128>(b);
        int64_t r = (s < lo) ? lo : (s > hi ? hi : static_cast<int64_t>(s));
#else
        int64_t s = a - b;
        bool ovPos = (a >= 0 && b < 0 && s < 0);
        bool ovNeg = (a < 0 && b > 0 && s >= 0);
        int64_t r = ovPos ? hi : ovNeg ? lo : (s < lo ? lo : (s > hi ? hi : s));
#endif
        return makeInt(N, r);
      }
    };

    /**
     * @brief @saturating_mul(a, b) — clamp the true product to [INT_MIN_N,
     * INT_MAX_N].
     */
    class SaturatingMulIntrinsic final : public InterpreterIntrinsic {
    public:
      Interpreter::RuntimeValue eval(
          const IntrinsicDecl &intr, const std::vector<Interpreter::RuntimeValue> &args
      ) const override {
        uint32_t N = *TypeUtils::getIntBitWidth(intr.retType);
        int64_t a = argSint(intr, args, 0), b = argSint(intr, args, 1);
        int64_t lo = intMinN(N), hi = intMaxN(N);
#if defined(__SIZEOF_INT128__)
        __int128 p = static_cast<__int128>(a) * static_cast<__int128>(b);
        int64_t r = (p < lo) ? lo : (p > hi ? hi : static_cast<int64_t>(p));
#else
        // No __int128: cover N <= 32 fully via int64 multiplication, and
        // fall back to a sign-only saturation for N == 64.
        if (N <= 32) {
          int64_t p = a * b;
          int64_t r = p < lo ? lo : (p > hi ? hi : p);
          return makeInt(N, r);
        }
        bool resPos = ((a >= 0) == (b >= 0));
        int64_t r;
        if (a == 0 || b == 0)
          r = 0;
        else if (resPos && a > hi / b)
          r = hi;
        else if (!resPos && a != 0 && (b < lo / a))
          r = lo;
        else
          r = a * b;
#endif
        return makeInt(N, r);
      }
    };

    /**
     * @brief @saturating_neg(x) — `-x`, clamped at INT_MAX_N when
     * `x == INT_MIN_N` (the only saturating case).
     */
    class SaturatingNegIntrinsic final : public InterpreterIntrinsic {
    public:
      Interpreter::RuntimeValue eval(
          const IntrinsicDecl &intr, const std::vector<Interpreter::RuntimeValue> &args
      ) const override {
        uint32_t N = *TypeUtils::getIntBitWidth(intr.retType);
        int64_t x = argSint(intr, args, 0);
        int64_t r = (x == intMinN(N)) ? intMaxN(N) : -x;
        return makeInt(N, r);
      }
    };

    /**
     * @brief @div_euclid(a, b) — Euclidean division; result rounds toward
     * −∞ when the remainder would be negative.  UB on `b == 0` and on
     * `a == INT_MIN_N && b == -1` (true quotient `-INT_MIN_N` does not
     * fit in iN).
     */
    class DivEuclidIntrinsic final : public InterpreterIntrinsic {
    public:
      Interpreter::RuntimeValue eval(
          const IntrinsicDecl &intr, const std::vector<Interpreter::RuntimeValue> &args
      ) const override {
        uint32_t N = *TypeUtils::getIntBitWidth(intr.retType);
        int64_t a = argSint(intr, args, 0), b = argSint(intr, args, 1);
        if (b == 0)
          throw UndefinedBehaviorError("UB: @div_euclid divisor is zero");
        if (a == intMinN(N) && b == -1)
          throw UndefinedBehaviorError(
              "UB: @div_euclid INT_MIN_N / -1 not representable in declared iN"
          );
        int64_t q = a / b;
        // Truncation toward zero -> Euclidean: if remainder is negative,
        // step the quotient toward -infinity.
        int64_t r = a - q * b;
        if (r < 0)
          q -= (b > 0) ? 1 : -1;
        return makeInt(N, q);
      }
    };

    /**
     * @brief @rem_euclid(a, b) — `a − @div_euclid(a, b) * b`; result is
     * always in `[0, |b|)`.  UB on `b == 0` and on `a == INT_MIN_N &&
     * b == -1` (matches @div_euclid).
     */
    class RemEuclidIntrinsic final : public InterpreterIntrinsic {
    public:
      Interpreter::RuntimeValue eval(
          const IntrinsicDecl &intr, const std::vector<Interpreter::RuntimeValue> &args
      ) const override {
        uint32_t N = *TypeUtils::getIntBitWidth(intr.retType);
        int64_t a = argSint(intr, args, 0), b = argSint(intr, args, 1);
        if (b == 0)
          throw UndefinedBehaviorError("UB: @rem_euclid divisor is zero");
        if (a == intMinN(N) && b == -1)
          throw UndefinedBehaviorError(
              "UB: @rem_euclid INT_MIN_N rem -1 paired with non-representable quotient"
          );
        int64_t r = a % b;
        if (r < 0)
          r += (b > 0) ? b : -b;
        return makeInt(N, r);
      }
    };

    // ── §12.6 Floating-point sign / bit ops (v0.2.2 extra batch D.1) ────

    /**
     * @brief @fabs(x) = |x|. Clears the IEEE 754 sign bit; never UB on the
     * SymIR finite-only domain.
     */
    class FabsIntrinsic final : public InterpreterIntrinsic {
    public:
      Interpreter::RuntimeValue eval(
          const IntrinsicDecl &intr, const std::vector<Interpreter::RuntimeValue> &args
      ) const override {
        uint32_t N = fpBitsOf(intr.retType);
        double x = argFloat(intr, args, 0);
        return makeFloat(N, std::fabs(x));
      }
    };

    /**
     * @brief @fneg(x) = -x. Flips the sign bit; preserves signed zero
     * (@fneg(+0.0) == -0.0).
     */
    class FnegIntrinsic final : public InterpreterIntrinsic {
    public:
      Interpreter::RuntimeValue eval(
          const IntrinsicDecl &intr, const std::vector<Interpreter::RuntimeValue> &args
      ) const override {
        uint32_t N = fpBitsOf(intr.retType);
        double x = argFloat(intr, args, 0);
        return makeFloat(N, -x);
      }
    };

    /**
     * @brief @copysign(x, y) returns x with the sign of y.
     */
    class CopysignIntrinsic final : public InterpreterIntrinsic {
    public:
      Interpreter::RuntimeValue eval(
          const IntrinsicDecl &intr, const std::vector<Interpreter::RuntimeValue> &args
      ) const override {
        uint32_t N = fpBitsOf(intr.retType);
        double x = argFloat(intr, args, 0), y = argFloat(intr, args, 1);
        return makeFloat(N, std::copysign(x, y));
      }
    };

    /**
     * @brief @signbit(x) returns 1 if x has its sign bit set (negative
     * finite or -0.0), else 0. i1 return.
     */
    class SignbitIntrinsic final : public InterpreterIntrinsic {
    public:
      Interpreter::RuntimeValue eval(
          const IntrinsicDecl & /*intr*/, const std::vector<Interpreter::RuntimeValue> &args
      ) const override {
        double x = args[0].floatVal;
        return makeInt(1, std::signbit(x) ? 1 : 0);
      }
    };

    /**
     * @brief @to_bits(x: fN) → iN: raw IEEE 754 bit pattern reinterpreted
     * as a signed integer of equal width. f32 → i32, f64 → i64.
     */
    class ToBitsIntrinsic final : public InterpreterIntrinsic {
    public:
      Interpreter::RuntimeValue eval(
          const IntrinsicDecl &intr, const std::vector<Interpreter::RuntimeValue> &args
      ) const override {
        uint32_t N = *TypeUtils::getIntBitWidth(intr.retType);
        if (N == 32) {
          float f = static_cast<float>(args[0].floatVal);
          uint32_t u;
          std::memcpy(&u, &f, 4);
          return makeInt(32, static_cast<int64_t>(static_cast<int32_t>(u)));
        }
        double d = args[0].floatVal;
        uint64_t u;
        std::memcpy(&u, &d, 8);
        return makeInt(64, static_cast<int64_t>(u));
      }
    };

    /**
     * @brief @from_bits(x: iN) → fN: raw IEEE 754 bit pattern reinterpreted
     * as a float of equal width. UB if the resulting value is non-finite
     * (matches the §2.9 finite-only domain).
     */
    class FromBitsIntrinsic final : public InterpreterIntrinsic {
    public:
      Interpreter::RuntimeValue eval(
          const IntrinsicDecl &intr, const std::vector<Interpreter::RuntimeValue> &args
      ) const override {
        uint32_t N = fpBitsOf(intr.retType);
        if (N == 32) {
          uint32_t u = static_cast<uint32_t>(args[0].intVal & 0xFFFFFFFFu);
          float f;
          std::memcpy(&f, &u, 4);
          if (!std::isfinite(f))
            throw UndefinedBehaviorError(
                "UB: @from_bits result is non-finite (spec §2.9 finite-only domain)"
            );
          return makeFloat(32, static_cast<double>(f));
        }
        uint64_t u = static_cast<uint64_t>(args[0].intVal);
        double d;
        std::memcpy(&d, &u, 8);
        if (!std::isfinite(d))
          throw UndefinedBehaviorError(
              "UB: @from_bits result is non-finite (spec §2.9 finite-only domain)"
          );
        return makeFloat(64, d);
      }
    };

    // ── Registry ────────────────────────────────────────────────────────────

    /**
     * @brief Centralized registry of all interpreter intrinsic implementations.
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
        // §12.5 — overflow-aware family.
        registry_[IntrinsicKind::WrappingAdd] = std::make_unique<WrappingAddIntrinsic>();
        registry_[IntrinsicKind::WrappingSub] = std::make_unique<WrappingSubIntrinsic>();
        registry_[IntrinsicKind::WrappingMul] = std::make_unique<WrappingMulIntrinsic>();
        registry_[IntrinsicKind::WrappingNeg] = std::make_unique<WrappingNegIntrinsic>();
        registry_[IntrinsicKind::WrappingShl] = std::make_unique<WrappingShlIntrinsic>();
        registry_[IntrinsicKind::WrappingShr] = std::make_unique<WrappingShrIntrinsic>();
        registry_[IntrinsicKind::SaturatingAdd] = std::make_unique<SaturatingAddIntrinsic>();
        registry_[IntrinsicKind::SaturatingSub] = std::make_unique<SaturatingSubIntrinsic>();
        registry_[IntrinsicKind::SaturatingMul] = std::make_unique<SaturatingMulIntrinsic>();
        registry_[IntrinsicKind::SaturatingNeg] = std::make_unique<SaturatingNegIntrinsic>();
        registry_[IntrinsicKind::DivEuclid] = std::make_unique<DivEuclidIntrinsic>();
        registry_[IntrinsicKind::RemEuclid] = std::make_unique<RemEuclidIntrinsic>();
        // §12.6 — FP sign / bit ops (v0.2.2 extra batch D.1).
        registry_[IntrinsicKind::Fabs] = std::make_unique<FabsIntrinsic>();
        registry_[IntrinsicKind::Fneg] = std::make_unique<FnegIntrinsic>();
        registry_[IntrinsicKind::Copysign] = std::make_unique<CopysignIntrinsic>();
        registry_[IntrinsicKind::Signbit] = std::make_unique<SignbitIntrinsic>();
        registry_[IntrinsicKind::ToBits] = std::make_unique<ToBitsIntrinsic>();
        registry_[IntrinsicKind::FromBits] = std::make_unique<FromBitsIntrinsic>();
      }

      std::unordered_map<IntrinsicKind, std::unique_ptr<InterpreterIntrinsic>> registry_;
    };

  } // namespace

  Interpreter::RuntimeValue Interpreter::callIntrinsic(
      const IntrinsicDecl &intr, const std::vector<RuntimeValue> &args, SourceSpan /*callSpan*/
  ) {
    auto kind = getIntrinsicKind(intr.name.name);
    if (kind) {
      if (auto impl = IntrinsicRegistry::get().lookup(*kind)) {
        return impl->eval(intr, args);
      }
    }

    throw std::runtime_error("Unknown intrinsic: " + intr.name.name);
  }

} // namespace symir
