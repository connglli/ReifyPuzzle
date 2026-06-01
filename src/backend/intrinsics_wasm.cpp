// [v0.2.2] WASM backend intrinsic helper emission.
//
// This file is the single source of truth for the WebAssembly Text Format
// (WAT) code emitted for every built-in SymIR intrinsic. The helpers use
// a widening-and-mask strategy: each iN operation is widened to the next
// WASM native width (i32 or i64), performed there, then sign-masked back
// to N bits. UB-preconditions abort via `unreachable`.
//
// Width conventions mirror intrinsics_c.cpp:
//   - Return-side `N`/`W`/`ity` are the return type's widening.
//   - For predicate intrinsics (@parity, @is_pow2), return is i1 (W=32,
//     ity=i32) while the input is bvN with its own widening. Per-param
//     types are emitted in the helper signature by emitIntrinsicHelper;
//     impls that touch param widths read them via `intr`.
//
// SymIR has no unsigned integer types. This file uses only:
//   - signed comparisons at the SymIR level (iW.lt_s / iW.gt_s / iW.eq...)
//   - bit-level operations (iW.shl, iW.shr_u, iW.and, iW.or, iW.xor,
//     iW.popcnt, iW.clz, iW.ctz) which act on bit patterns, not as
//     unsigned-integer numerics.
//   - widening between i32 ↔ i64 only where required by missing native
//     ops at narrow widths (analogous to "no i3 exists, must use i8"
//     in the C backend).
//
// To add a new intrinsic:
//   1. Add its IntrinsicKind to include/analysis/intrinsics.hpp.
//   2. Implement a subclass of WasmIntrinsic and register it below.
//
// Note: float literal format intentionally diverges from symir::formatDouble.
// WAT has its own grammar rules. See AGENTS.md §FP-serialization-invariant.

#include "backend/wasm_backend.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include "analysis/intrinsics.hpp"
#include "analysis/type_utils.hpp"

namespace symir {

  class WasmIntrinsic {
  public:
    virtual ~WasmIntrinsic() = default;

    virtual void emit(
        WasmBackend &backend, const IntrinsicDecl &intr, uint32_t N, uint32_t W,
        const std::string &ity
    ) const = 0;

    /**
     * @brief Optional: declares scratch (`local`) variables used by this impl.
     */
    virtual void declareLocals(
        WasmBackend &, const IntrinsicDecl &, uint32_t, uint32_t, const std::string &
    ) const {}

  protected:
    static std::ostream &out(WasmBackend &backend);
    static void indent(WasmBackend &backend);
    static void incrIndent(WasmBackend &backend);
    static void decrIndent(WasmBackend &backend);

    static uint32_t widen(uint32_t N) { return (N <= 32) ? 32 : 64; }

    static std::string ityOf(uint32_t W) { return (W == 32) ? "i32" : "i64"; }

    static uint32_t paramN(const IntrinsicDecl &intr, size_t i) {
      auto pb = TypeUtils::getIntBitWidth(intr.params[i].type);
      return pb ? *pb : 32;
    }

    static void pushArg(WasmBackend &backend, size_t i) {
      indent(backend);
      out(backend) << "local.get $a" << i << "\n";
    }

    /**
     * @brief Sign-extend the top of stack so its low N bits encode a signed
     * iN value, padded out to W bits.
     */
    static void sextN(WasmBackend &backend, uint32_t N, uint32_t W, const std::string &ity) {
      if (N == W)
        return;
      indent(backend);
      out(backend) << ity << ".const " << (W - N) << "\n";
      indent(backend);
      out(backend) << ity << ".shl\n";
      indent(backend);
      out(backend) << ity << ".const " << (W - N) << "\n";
      indent(backend);
      out(backend) << ity << ".shr_s\n";
    }

    /**
     * @brief Push an N-bit mask onto the stack (-1 if N == W, else (1 << N) - 1).
     */
    static void pushMask(WasmBackend &backend, uint32_t N, uint32_t W, const std::string &ity) {
      if (N == W) {
        indent(backend);
        out(backend) << ity << ".const -1\n";
      } else {
        indent(backend);
        out(backend) << ity << ".const " << ((uint64_t(1) << N) - 1) << "\n";
      }
    }

    static void declLocal(WasmBackend &backend, const std::string &name, const std::string &ity) {
      indent(backend);
      out(backend) << "(local $" << name << " " << ity << ")\n";
    }

    static void unreachableIfTop(WasmBackend &backend) {
      indent(backend);
      out(backend) << "if\n";
      incrIndent(backend);
      indent(backend);
      out(backend) << "unreachable\n";
      decrIndent(backend);
      indent(backend);
      out(backend) << "end\n";
    }
  };

  struct WasmIntrinsicRegistry {
    static std::ostream &out(WasmBackend &backend) { return backend.out_; }

    static void indent(WasmBackend &backend) { backend.indent(); }

    static void incrIndent(WasmBackend &backend) { backend.indent_level_++; }

    static void decrIndent(WasmBackend &backend) { backend.indent_level_--; }

    using WasmIntrinsicGenFn = std::unique_ptr<WasmIntrinsic>;
    static const std::unordered_map<IntrinsicKind, WasmIntrinsicGenFn> &getRegistry();
  };

  std::ostream &WasmIntrinsic::out(WasmBackend &backend) {
    return WasmIntrinsicRegistry::out(backend);
  }

  void WasmIntrinsic::indent(WasmBackend &backend) { WasmIntrinsicRegistry::indent(backend); }

  void WasmIntrinsic::incrIndent(WasmBackend &backend) {
    WasmIntrinsicRegistry::incrIndent(backend);
  }

  void WasmIntrinsic::decrIndent(WasmBackend &backend) {
    WasmIntrinsicRegistry::decrIndent(backend);
  }

  namespace {

    // ── §12.1 Arithmetic intrinsics ─────────────────────────────────────────

    class AbsIntrinsic final : public WasmIntrinsic {
    public:
      void emit(
          WasmBackend &backend, const IntrinsicDecl &, uint32_t N, uint32_t W,
          const std::string &ity
      ) const override {
        int64_t int_min_N = (N == 64) ? INT64_MIN : -(INT64_C(1) << (N - 1));
        pushArg(backend, 0);
        indent(backend);
        out(backend) << ity << ".const " << int_min_N << "\n";
        indent(backend);
        out(backend) << ity << ".eq\n";
        unreachableIfTop(backend);

        indent(backend);
        out(backend) << ity << ".const 0\n";
        pushArg(backend, 0);
        indent(backend);
        out(backend) << ity << ".sub\n";
        pushArg(backend, 0);
        pushArg(backend, 0);
        indent(backend);
        out(backend) << ity << ".const 0\n";
        indent(backend);
        out(backend) << ity << ".lt_s\n";
        indent(backend);
        out(backend) << "select\n";
        sextN(backend, N, W, ity);
      }
    };

    class MinIntrinsic final : public WasmIntrinsic {
    public:
      void emit(
          WasmBackend &backend, const IntrinsicDecl &, uint32_t N, uint32_t W,
          const std::string &ity
      ) const override {
        pushArg(backend, 0);
        pushArg(backend, 1);
        pushArg(backend, 0);
        pushArg(backend, 1);
        indent(backend);
        out(backend) << ity << ".lt_s\n";
        indent(backend);
        out(backend) << "select\n";
        sextN(backend, N, W, ity);
      }
    };

    class MaxIntrinsic final : public WasmIntrinsic {
    public:
      void emit(
          WasmBackend &backend, const IntrinsicDecl &, uint32_t N, uint32_t W,
          const std::string &ity
      ) const override {
        pushArg(backend, 0);
        pushArg(backend, 1);
        pushArg(backend, 0);
        pushArg(backend, 1);
        indent(backend);
        out(backend) << ity << ".gt_s\n";
        indent(backend);
        out(backend) << "select\n";
        sextN(backend, N, W, ity);
      }
    };

    // ── §12.2 Bit-counting intrinsics ───────────────────────────────────────

    class PopcountIntrinsic final : public WasmIntrinsic {
    public:
      void emit(
          WasmBackend &backend, const IntrinsicDecl &, uint32_t N, uint32_t W,
          const std::string &ity
      ) const override {
        pushArg(backend, 0);
        pushMask(backend, N, W, ity);
        indent(backend);
        out(backend) << ity << ".and\n";
        indent(backend);
        out(backend) << ity << ".popcnt\n";
        sextN(backend, N, W, ity);
      }
    };

    class ClzIntrinsic final : public WasmIntrinsic {
    public:
      void declareLocals(
          WasmBackend &b, const IntrinsicDecl &, uint32_t, uint32_t, const std::string &ity
      ) const override {
        declLocal(b, "tmp0", ity);
      }

      void emit(
          WasmBackend &backend, const IntrinsicDecl &, uint32_t N, uint32_t W,
          const std::string &ity
      ) const override {
        pushArg(backend, 0);
        pushMask(backend, N, W, ity);
        indent(backend);
        out(backend) << ity << ".and\n";
        indent(backend);
        out(backend) << "local.tee $tmp0\n";
        indent(backend);
        out(backend) << ity << ".eqz\n";
        unreachableIfTop(backend);
        indent(backend);
        out(backend) << "local.get $tmp0\n";
        indent(backend);
        out(backend) << ity << ".clz\n";
        if (N != W) {
          indent(backend);
          out(backend) << ity << ".const " << (W - N) << "\n";
          indent(backend);
          out(backend) << ity << ".sub\n";
        }
        sextN(backend, N, W, ity);
      }
    };

    class CtzIntrinsic final : public WasmIntrinsic {
    public:
      void declareLocals(
          WasmBackend &b, const IntrinsicDecl &, uint32_t, uint32_t, const std::string &ity
      ) const override {
        declLocal(b, "tmp0", ity);
      }

      void emit(
          WasmBackend &backend, const IntrinsicDecl &, uint32_t N, uint32_t W,
          const std::string &ity
      ) const override {
        pushArg(backend, 0);
        pushMask(backend, N, W, ity);
        indent(backend);
        out(backend) << ity << ".and\n";
        indent(backend);
        out(backend) << "local.tee $tmp0\n";
        indent(backend);
        out(backend) << ity << ".eqz\n";
        unreachableIfTop(backend);
        indent(backend);
        out(backend) << "local.get $tmp0\n";
        indent(backend);
        out(backend) << ity << ".ctz\n";
        sextN(backend, N, W, ity);
      }
    };

    // ── §12.3 Integer extras (v0.2.2 extra batch A) ────────────────────────────────

    /**
     * @brief @abs_diff(a, b) = |a - b|, UB if not representable in iN.
     * For N <= 32, widen both args to i64, subtract safely, then check
     * |s| > INT_MAX_N, then narrow. For N == 64, detect signed-iW overflow
     * directly without any unsigned reinterpretation.
     */
    class AbsDiffIntrinsic final : public WasmIntrinsic {
    public:
      void declareLocals(
          WasmBackend &b, const IntrinsicDecl &, uint32_t N, uint32_t W, const std::string &ity
      ) const override {
        if (N <= 32) {
          declLocal(b, "s64", "i64");
          declLocal(b, "r64", "i64");
        } else {
          declLocal(b, "s", ity);
          declLocal(b, "r", ity);
        }
        (void) W;
      }

      void emit(
          WasmBackend &backend, const IntrinsicDecl &, uint32_t N, uint32_t W,
          const std::string &ity
      ) const override {
        int64_t maxN = (N == 64) ? INT64_MAX : ((INT64_C(1) << (N - 1)) - 1);
        if (N <= 32) {
          // i64-widened subtraction; no overflow possible since |i32 - i32| <= 2^32 - 1.
          pushArg(backend, 0);
          indent(backend);
          out(backend) << "i64.extend_i32_s\n";
          pushArg(backend, 1);
          indent(backend);
          out(backend) << "i64.extend_i32_s\n";
          indent(backend);
          out(backend) << "i64.sub\n";
          indent(backend);
          out(backend) << "local.set $s64\n";
          // r64 = |s64| via select
          indent(backend);
          out(backend) << "i64.const 0\n";
          indent(backend);
          out(backend) << "local.get $s64\n";
          indent(backend);
          out(backend) << "i64.sub\n";
          indent(backend);
          out(backend) << "local.get $s64\n";
          indent(backend);
          out(backend) << "local.get $s64\n";
          indent(backend);
          out(backend) << "i64.const 0\n";
          indent(backend);
          out(backend) << "i64.lt_s\n";
          indent(backend);
          out(backend) << "select\n";
          indent(backend);
          out(backend) << "local.tee $r64\n";
          indent(backend);
          out(backend) << "i64.const " << maxN << "\n";
          indent(backend);
          out(backend) << "i64.gt_s\n";
          unreachableIfTop(backend);
          // narrow & sext
          indent(backend);
          out(backend) << "local.get $r64\n";
          indent(backend);
          out(backend) << "i32.wrap_i64\n";
          sextN(backend, N, W, ity);
        } else {
          // N == 64 (so W = 64). Detect overflow via the two's-complement
          // identity: a - b overflows signed iff (a XOR b) has the high bit
          // set AND (a XOR s) has the high bit set, where s = a - b.
          pushArg(backend, 0);
          pushArg(backend, 1);
          indent(backend);
          out(backend) << ity << ".sub\n";
          indent(backend);
          out(backend) << "local.set $s\n";
          // overflow test
          pushArg(backend, 0);
          pushArg(backend, 1);
          indent(backend);
          out(backend) << ity << ".xor\n";
          pushArg(backend, 0);
          indent(backend);
          out(backend) << "local.get $s\n";
          indent(backend);
          out(backend) << ity << ".xor\n";
          indent(backend);
          out(backend) << ity << ".and\n";
          indent(backend);
          out(backend) << ity << ".const 0\n";
          indent(backend);
          out(backend) << ity << ".lt_s\n";
          unreachableIfTop(backend);
          // r = |s| via select
          indent(backend);
          out(backend) << ity << ".const 0\n";
          indent(backend);
          out(backend) << "local.get $s\n";
          indent(backend);
          out(backend) << ity << ".sub\n";
          indent(backend);
          out(backend) << "local.get $s\n";
          indent(backend);
          out(backend) << "local.get $s\n";
          indent(backend);
          out(backend) << ity << ".const 0\n";
          indent(backend);
          out(backend) << ity << ".lt_s\n";
          indent(backend);
          out(backend) << "select\n";
          indent(backend);
          out(backend) << "local.tee $r\n";
          // r is in [0, INT_MAX_W=INT_MAX_64]. For N=W=64, no further check
          // required. Just return.
          indent(backend);
          out(backend) << "drop\n";
          indent(backend);
          out(backend) << "local.get $r\n";
        }
      }
    };

    class SignumIntrinsic final : public WasmIntrinsic {
    public:
      void emit(
          WasmBackend &backend, const IntrinsicDecl &, uint32_t N, uint32_t W,
          const std::string &ity
      ) const override {
        // r = (x > 0) - (x < 0)
        pushArg(backend, 0);
        indent(backend);
        out(backend) << ity << ".const 0\n";
        indent(backend);
        out(backend) << ity << ".gt_s\n";
        pushArg(backend, 0);
        indent(backend);
        out(backend) << ity << ".const 0\n";
        indent(backend);
        out(backend) << ity << ".lt_s\n";
        indent(backend);
        out(backend) << ity << ".sub\n";
        // [v0.2.2] Predicate: N=1 (i1) — stored as literal 0/1, not sign-extended.
        if (N > 1)
          sextN(backend, N, W, ity);
      }
    };

    class ClampIntrinsic final : public WasmIntrinsic {
    public:
      void emit(
          WasmBackend &backend, const IntrinsicDecl &, uint32_t N, uint32_t W,
          const std::string &ity
      ) const override {
        // UB if lo > hi
        pushArg(backend, 1);
        pushArg(backend, 2);
        indent(backend);
        out(backend) << ity << ".gt_s\n";
        unreachableIfTop(backend);
        // r = (v < lo) ? lo : ((v > hi) ? hi : v)
        pushArg(backend, 1); // lo
        // compute upper = (v > hi) ? hi : v on top
        pushArg(backend, 2); // hi
        pushArg(backend, 0); // v
        pushArg(backend, 0);
        pushArg(backend, 2);
        indent(backend);
        out(backend) << ity << ".gt_s\n";
        indent(backend);
        out(backend) << "select\n"; // upper
        pushArg(backend, 0);
        pushArg(backend, 1);
        indent(backend);
        out(backend) << ity << ".lt_s\n";
        indent(backend);
        out(backend) << "select\n"; // r
        sextN(backend, N, W, ity);
      }
    };

    class MidpointIntrinsic final : public WasmIntrinsic {
    public:
      void declareLocals(
          WasmBackend &b, const IntrinsicDecl &, uint32_t N, uint32_t, const std::string &
      ) const override {
        if (N == 64)
          declLocal(b, "tmp64", "i64");
      }

      void emit(
          WasmBackend &backend, const IntrinsicDecl &, uint32_t N, uint32_t W,
          const std::string &ity
      ) const override {
        if (N <= 32) {
          // i64-widened add and signed division.
          pushArg(backend, 0);
          indent(backend);
          out(backend) << "i64.extend_i32_s\n";
          pushArg(backend, 1);
          indent(backend);
          out(backend) << "i64.extend_i32_s\n";
          indent(backend);
          out(backend) << "i64.add\n";
          indent(backend);
          out(backend) << "i64.const 2\n";
          indent(backend);
          out(backend) << "i64.div_s\n";
          indent(backend);
          out(backend) << "i32.wrap_i64\n";
          sextN(backend, N, W, ity);
        } else if (N <= 63) {
          // 33..63 — sum fits in i64.
          pushArg(backend, 0);
          pushArg(backend, 1);
          indent(backend);
          out(backend) << "i64.add\n";
          indent(backend);
          out(backend) << "i64.const 2\n";
          indent(backend);
          out(backend) << "i64.div_s\n";
          sextN(backend, N, W, ity);
        } else {
          // N == 64: bit-level midpoint truncating toward zero:
          //   floor_avg = (a & b) + ((a ^ b) >> 1)  ;; ashr (signed) for floor
          //   adjust by +1 if ((a ^ b) & 1) AND floor_avg < 0  (truncate ⇒ 0).
          pushArg(backend, 0);
          pushArg(backend, 1);
          indent(backend);
          out(backend) << "i64.and\n";
          pushArg(backend, 0);
          pushArg(backend, 1);
          indent(backend);
          out(backend) << "i64.xor\n";
          indent(backend);
          out(backend) << "i64.const 1\n";
          indent(backend);
          out(backend) << "i64.shr_s\n";
          indent(backend);
          out(backend) << "i64.add\n";
          indent(backend);
          out(backend) << "local.set $tmp64\n";
          // adjust: if ((a ^ b) & 1) and tmp64 < 0, tmp64 += 1
          pushArg(backend, 0);
          pushArg(backend, 1);
          indent(backend);
          out(backend) << "i64.xor\n";
          indent(backend);
          out(backend) << "i64.const 1\n";
          indent(backend);
          out(backend) << "i64.and\n";
          indent(backend);
          out(backend) << "local.get $tmp64\n";
          indent(backend);
          out(backend) << "i64.const 0\n";
          indent(backend);
          out(backend) << "i64.lt_s\n";
          indent(backend);
          out(backend) << "i64.extend_i32_u\n";
          indent(backend);
          out(backend) << "i64.and\n";
          indent(backend);
          out(backend) << "local.get $tmp64\n";
          indent(backend);
          out(backend) << "i64.add\n";
          // already i64; W == 64, no sextN.
        }
      }
    };

    // ── §12.4 Bit-manipulation (v0.2.2 extra batch B) ──────────────────────────────

    class ParityIntrinsic final : public WasmIntrinsic {
    public:
      void emit(
          WasmBackend &backend, const IntrinsicDecl &intr, uint32_t N, uint32_t W,
          const std::string &ity
      ) const override {
        (void) N;
        uint32_t pN = paramN(intr, 0);
        uint32_t pW = widen(pN);
        std::string pity = ityOf(pW);
        pushArg(backend, 0);
        // mask to param-N bits (bit-level mask, not unsigned interpretation)
        if (pN < pW) {
          indent(backend);
          out(backend) << pity << ".const " << ((uint64_t(1) << pN) - 1) << "\n";
          indent(backend);
          out(backend) << pity << ".and\n";
        }
        indent(backend);
        out(backend) << pity << ".popcnt\n";
        // popcnt result is bit-level; narrow/widen to ret-ity if widths differ.
        if (pW == 64 && W == 32) {
          indent(backend);
          out(backend) << "i32.wrap_i64\n";
        } else if (pW == 32 && W == 64) {
          indent(backend);
          out(backend) << "i64.extend_i32_s\n";
        }
        indent(backend);
        out(backend) << ity << ".const 1\n";
        indent(backend);
        out(backend) << ity << ".and\n";
        // No sextN: SymIR i1 is stored as the literal 0 / 1 (matches `cmp`),
        // not sign-extended.
      }
    };

    class BswapIntrinsic final : public WasmIntrinsic {
    public:
      void declareLocals(
          WasmBackend &b, const IntrinsicDecl &, uint32_t, uint32_t, const std::string &ity
      ) const override {
        declLocal(b, "u", ity);
        declLocal(b, "r", ity);
      }

      void emit(
          WasmBackend &backend, const IntrinsicDecl &, uint32_t N, uint32_t W,
          const std::string &ity
      ) const override {
        pushArg(backend, 0);
        if (N < W) {
          pushMask(backend, N, W, ity);
          indent(backend);
          out(backend) << ity << ".and\n";
        }
        indent(backend);
        out(backend) << "local.set $u\n";
        indent(backend);
        out(backend) << ity << ".const 0\n";
        indent(backend);
        out(backend) << "local.set $r\n";
        uint32_t nbytes = N / 8;
        for (uint32_t i = 0; i < nbytes; ++i) {
          indent(backend);
          out(backend) << "local.get $u\n";
          indent(backend);
          out(backend) << ity << ".const " << (i * 8) << "\n";
          indent(backend);
          out(backend) << ity << ".shr_u\n";
          indent(backend);
          out(backend) << ity << ".const 255\n";
          indent(backend);
          out(backend) << ity << ".and\n";
          indent(backend);
          out(backend) << ity << ".const " << ((nbytes - 1 - i) * 8) << "\n";
          indent(backend);
          out(backend) << ity << ".shl\n";
          indent(backend);
          out(backend) << "local.get $r\n";
          indent(backend);
          out(backend) << ity << ".or\n";
          indent(backend);
          out(backend) << "local.set $r\n";
        }
        indent(backend);
        out(backend) << "local.get $r\n";
        sextN(backend, N, W, ity);
      }
    };

    class BitreverseIntrinsic final : public WasmIntrinsic {
    public:
      void declareLocals(
          WasmBackend &b, const IntrinsicDecl &, uint32_t, uint32_t, const std::string &ity
      ) const override {
        declLocal(b, "u", ity);
        declLocal(b, "r", ity);
      }

      void emit(
          WasmBackend &backend, const IntrinsicDecl &, uint32_t N, uint32_t W,
          const std::string &ity
      ) const override {
        pushArg(backend, 0);
        if (N < W) {
          pushMask(backend, N, W, ity);
          indent(backend);
          out(backend) << ity << ".and\n";
        }
        indent(backend);
        out(backend) << "local.set $u\n";
        indent(backend);
        out(backend) << ity << ".const 0\n";
        indent(backend);
        out(backend) << "local.set $r\n";
        for (uint32_t i = 0; i < N; ++i) {
          indent(backend);
          out(backend) << "local.get $u\n";
          indent(backend);
          out(backend) << ity << ".const " << i << "\n";
          indent(backend);
          out(backend) << ity << ".shr_u\n";
          indent(backend);
          out(backend) << ity << ".const 1\n";
          indent(backend);
          out(backend) << ity << ".and\n";
          indent(backend);
          out(backend) << ity << ".const " << (N - 1 - i) << "\n";
          indent(backend);
          out(backend) << ity << ".shl\n";
          indent(backend);
          out(backend) << "local.get $r\n";
          indent(backend);
          out(backend) << ity << ".or\n";
          indent(backend);
          out(backend) << "local.set $r\n";
        }
        indent(backend);
        out(backend) << "local.get $r\n";
        sextN(backend, N, W, ity);
      }
    };

    /**
     * @brief Emit @rotl or @rotr body. Rotation is a bit-permutation; we use
     * shr_u as the bit-level shift-without-sign-propagation primitive (it is
     * not an unsigned-numeric interpretation).
     */
    static void
    emitRotation(WasmBackend &b, bool leftRot, uint32_t N, uint32_t W, const std::string &ity) {
      // UB: a1 < 0 || a1 >= N
      WasmIntrinsicRegistry::indent(b);
      WasmIntrinsicRegistry::out(b) << "local.get $a1\n";
      WasmIntrinsicRegistry::indent(b);
      WasmIntrinsicRegistry::out(b) << ity << ".const 0\n";
      WasmIntrinsicRegistry::indent(b);
      WasmIntrinsicRegistry::out(b) << ity << ".lt_s\n";
      WasmIntrinsicRegistry::indent(b);
      WasmIntrinsicRegistry::out(b) << "local.get $a1\n";
      WasmIntrinsicRegistry::indent(b);
      WasmIntrinsicRegistry::out(b) << ity << ".const " << N << "\n";
      WasmIntrinsicRegistry::indent(b);
      WasmIntrinsicRegistry::out(b) << ity << ".ge_s\n";
      WasmIntrinsicRegistry::indent(b);
      WasmIntrinsicRegistry::out(b) << "i32.or\n";
      WasmIntrinsicRegistry::indent(b);
      WasmIntrinsicRegistry::out(b) << "if\n";
      WasmIntrinsicRegistry::incrIndent(b);
      WasmIntrinsicRegistry::indent(b);
      WasmIntrinsicRegistry::out(b) << "unreachable\n";
      WasmIntrinsicRegistry::decrIndent(b);
      WasmIntrinsicRegistry::indent(b);
      WasmIntrinsicRegistry::out(b) << "end\n";

      // u = a0 [& mask if N < W]
      WasmIntrinsicRegistry::indent(b);
      WasmIntrinsicRegistry::out(b) << "local.get $a0\n";
      if (N < W) {
        WasmIntrinsicRegistry::indent(b);
        WasmIntrinsicRegistry::out(b) << ity << ".const " << ((uint64_t(1) << N) - 1) << "\n";
        WasmIntrinsicRegistry::indent(b);
        WasmIntrinsicRegistry::out(b) << ity << ".and\n";
      }
      WasmIntrinsicRegistry::indent(b);
      WasmIntrinsicRegistry::out(b) << "local.set $u\n";

      // left_part = u (shl|shr_u) n  [&mask if shl and N<W]
      WasmIntrinsicRegistry::indent(b);
      WasmIntrinsicRegistry::out(b) << "local.get $u\n";
      WasmIntrinsicRegistry::indent(b);
      WasmIntrinsicRegistry::out(b) << "local.get $a1\n";
      WasmIntrinsicRegistry::indent(b);
      WasmIntrinsicRegistry::out(b) << ity << (leftRot ? ".shl\n" : ".shr_u\n");
      if (leftRot && N < W) {
        WasmIntrinsicRegistry::indent(b);
        WasmIntrinsicRegistry::out(b) << ity << ".const " << ((uint64_t(1) << N) - 1) << "\n";
        WasmIntrinsicRegistry::indent(b);
        WasmIntrinsicRegistry::out(b) << ity << ".and\n";
      }
      // right_part = u (shr_u|shl) (N - n)
      WasmIntrinsicRegistry::indent(b);
      WasmIntrinsicRegistry::out(b) << "local.get $u\n";
      WasmIntrinsicRegistry::indent(b);
      WasmIntrinsicRegistry::out(b) << ity << ".const " << N << "\n";
      WasmIntrinsicRegistry::indent(b);
      WasmIntrinsicRegistry::out(b) << "local.get $a1\n";
      WasmIntrinsicRegistry::indent(b);
      WasmIntrinsicRegistry::out(b) << ity << ".sub\n";
      WasmIntrinsicRegistry::indent(b);
      WasmIntrinsicRegistry::out(b) << ity << (leftRot ? ".shr_u\n" : ".shl\n");
      if (!leftRot && N < W) {
        WasmIntrinsicRegistry::indent(b);
        WasmIntrinsicRegistry::out(b) << ity << ".const " << ((uint64_t(1) << N) - 1) << "\n";
        WasmIntrinsicRegistry::indent(b);
        WasmIntrinsicRegistry::out(b) << ity << ".and\n";
      }
      WasmIntrinsicRegistry::indent(b);
      WasmIntrinsicRegistry::out(b) << ity << ".or\n";

      // For n == 0: WASM shifts reduce shift count mod W. shl by 0 → u; shr_u
      // by (N - 0) = N → either u (if N == W) or 0 (since u was masked). OR
      // yields u in both cases — no extra special case needed.
    }

    class RotlIntrinsic final : public WasmIntrinsic {
    public:
      void declareLocals(
          WasmBackend &b, const IntrinsicDecl &, uint32_t, uint32_t, const std::string &ity
      ) const override {
        declLocal(b, "u", ity);
      }

      void emit(
          WasmBackend &backend, const IntrinsicDecl &, uint32_t N, uint32_t W,
          const std::string &ity
      ) const override {
        emitRotation(backend, true, N, W, ity);
        sextN(backend, N, W, ity);
      }
    };

    class RotrIntrinsic final : public WasmIntrinsic {
    public:
      void declareLocals(
          WasmBackend &b, const IntrinsicDecl &, uint32_t, uint32_t, const std::string &ity
      ) const override {
        declLocal(b, "u", ity);
      }

      void emit(
          WasmBackend &backend, const IntrinsicDecl &, uint32_t N, uint32_t W,
          const std::string &ity
      ) const override {
        emitRotation(backend, false, N, W, ity);
        sextN(backend, N, W, ity);
      }
    };

    class IsPow2Intrinsic final : public WasmIntrinsic {
    public:
      void emit(
          WasmBackend &backend, const IntrinsicDecl &intr, uint32_t N, uint32_t W,
          const std::string &ity
      ) const override {
        uint32_t pN = paramN(intr, 0);
        uint32_t pW = widen(pN);
        std::string pity = ityOf(pW);
        (void) N;
        (void) pN;
        (void) ity;
        // (a0 > 0) — pity.gt_s yields i32
        pushArg(backend, 0);
        indent(backend);
        out(backend) << pity << ".const 0\n";
        indent(backend);
        out(backend) << pity << ".gt_s\n";
        // (a0 & (a0 - 1)) == 0 — pity.eqz yields i32
        pushArg(backend, 0);
        pushArg(backend, 0);
        indent(backend);
        out(backend) << pity << ".const 1\n";
        indent(backend);
        out(backend) << pity << ".sub\n";
        indent(backend);
        out(backend) << pity << ".and\n";
        indent(backend);
        out(backend) << pity << ".eqz\n";
        // AND the two i32 predicates → i32 holding 0 or 1
        indent(backend);
        out(backend) << "i32.and\n";
        // For i1 return, W is always 32, so the i32 0/1 on stack is the
        // result. Defensive widening if a future change makes W == 64.
        if (W == 64) {
          indent(backend);
          out(backend) << "i64.extend_i32_s\n";
        }
        // No sextN: SymIR i1 is stored as 0 / 1, not sign-extended.
      }
    };

    class Ilog2Intrinsic final : public WasmIntrinsic {
    public:
      void declareLocals(
          WasmBackend &b, const IntrinsicDecl &, uint32_t, uint32_t, const std::string &ity
      ) const override {
        declLocal(b, "u", ity);
      }

      void emit(
          WasmBackend &backend, const IntrinsicDecl &, uint32_t N, uint32_t W,
          const std::string &ity
      ) const override {
        // UB if a0 <= 0
        pushArg(backend, 0);
        indent(backend);
        out(backend) << ity << ".const 0\n";
        indent(backend);
        out(backend) << ity << ".le_s\n";
        unreachableIfTop(backend);

        // u = a0 [& mask if N < W]
        pushArg(backend, 0);
        if (N < W) {
          pushMask(backend, N, W, ity);
          indent(backend);
          out(backend) << ity << ".and\n";
        }
        indent(backend);
        out(backend) << "local.set $u\n";

        // ilog2 = (N - 1) - (clz(u) - (W - N)) = (W - 1) - clz(u)
        // Build (W - 1) - clz(u) on the stack:
        indent(backend);
        out(backend) << ity << ".const " << (W - 1) << "\n";
        indent(backend);
        out(backend) << "local.get $u\n";
        indent(backend);
        out(backend) << ity << ".clz\n";
        indent(backend);
        out(backend) << ity << ".sub\n";
        sextN(backend, N, W, ity);
      }
    };

  } // namespace

  const std::unordered_map<IntrinsicKind, WasmIntrinsicRegistry::WasmIntrinsicGenFn> &
  WasmIntrinsicRegistry::getRegistry() {
    static const std::unordered_map<IntrinsicKind, WasmIntrinsicGenFn> registry = []() {
      std::unordered_map<IntrinsicKind, WasmIntrinsicGenFn> r;
      r[IntrinsicKind::Abs] = std::make_unique<AbsIntrinsic>();
      r[IntrinsicKind::Min] = std::make_unique<MinIntrinsic>();
      r[IntrinsicKind::Max] = std::make_unique<MaxIntrinsic>();
      r[IntrinsicKind::Popcount] = std::make_unique<PopcountIntrinsic>();
      r[IntrinsicKind::Clz] = std::make_unique<ClzIntrinsic>();
      r[IntrinsicKind::Ctz] = std::make_unique<CtzIntrinsic>();
      r[IntrinsicKind::AbsDiff] = std::make_unique<AbsDiffIntrinsic>();
      r[IntrinsicKind::Signum] = std::make_unique<SignumIntrinsic>();
      r[IntrinsicKind::Clamp] = std::make_unique<ClampIntrinsic>();
      r[IntrinsicKind::Midpoint] = std::make_unique<MidpointIntrinsic>();
      r[IntrinsicKind::Parity] = std::make_unique<ParityIntrinsic>();
      r[IntrinsicKind::Bswap] = std::make_unique<BswapIntrinsic>();
      r[IntrinsicKind::Bitreverse] = std::make_unique<BitreverseIntrinsic>();
      r[IntrinsicKind::Rotl] = std::make_unique<RotlIntrinsic>();
      r[IntrinsicKind::Rotr] = std::make_unique<RotrIntrinsic>();
      r[IntrinsicKind::IsPow2] = std::make_unique<IsPow2Intrinsic>();
      r[IntrinsicKind::Ilog2] = std::make_unique<Ilog2Intrinsic>();
      return r;
    }();
    return registry;
  }

  std::string WasmBackend::intrinsicHelperName(const std::string &intrName, uint32_t bits) const {
    std::string base = intrName;
    if (!base.empty() && base[0] == '@')
      base.erase(0, 1);
    return "$_symir_" + base + "_i" + std::to_string(bits);
  }

  void WasmBackend::emitIntrinsicHelper(const IntrinsicDecl &intr) {
    auto rb = getIntWidth(intr.retType);
    if (rb == 0)
      return;
    uint32_t N = rb;
    uint32_t W = (N <= 32) ? 32 : 64;
    std::string ity = (W == 32) ? "i32" : "i64";
    std::string name = intrinsicHelperName(intr.name.name, N);

    indent();
    out_ << "(func " << name;
    for (size_t i = 0; i < intr.params.size(); ++i) {
      uint32_t pN = getIntWidth(intr.params[i].type);
      if (pN == 0)
        pN = 32;
      uint32_t pW = (pN <= 32) ? 32 : 64;
      std::string pity = (pW == 32) ? "i32" : "i64";
      out_ << " (param $a" << i << " " << pity << ")";
    }
    out_ << " (result " << ity << ")\n";
    indent_level_++;

    auto kind = getIntrinsicKind(intr.name.name);
    if (kind) {
      const auto &registry = WasmIntrinsicRegistry::getRegistry();
      auto it = registry.find(*kind);
      if (it != registry.end()) {
        it->second->declareLocals(*this, intr, N, W, ity);
        it->second->emit(*this, intr, N, W, ity);
        indent_level_--;
        indent();
        out_ << ")\n";
        return;
      }
    }

    indent();
    out_ << "unreachable\n";
    indent_level_--;
    indent();
    out_ << ")\n";
  }

} // namespace symir
