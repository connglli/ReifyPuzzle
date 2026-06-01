// [v0.2.2] C backend intrinsic helper emission.
//
// This file is the single source of truth for the C code emitted for
// every built-in SymIR intrinsic. The helpers use a widening-and-mask
// strategy: each iN operation is widened to the next machine integer
// width (8/16/32/64), performed there, then sign-masked back to N bits.
// UB-preconditions abort via __builtin_trap.
//
// Width conventions:
//   - Return-side N/W/sty/uty are the *return* type's widening.
//   - For predicate intrinsics (@parity, @is_pow2), the return is i1
//     (W=8, sty=int8_t) but the input is iN_param with its own widening.
//     emitIntrinsicHelper computes per-param types in the helper
//     signature; impls that touch parameter widths read them via `intr`.
//
// To add a new intrinsic:
//   1. Add its IntrinsicKind to include/analysis/intrinsics.hpp.
//   2. Implement a subclass of CIntrinsic and register it below.

#include "backend/c_backend.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include "analysis/intrinsics.hpp"
#include "analysis/type_utils.hpp"

namespace symir {

  /**
   * @brief Abstract base class for C code-generation of intrinsics.
   * Subclasses generate C function bodies to emulate SymIR standard intrinsics.
   */
  class CIntrinsic {
  public:
    virtual ~CIntrinsic() = default;

    /**
     * @brief Generates C statements inside the helper function body.
     * @param backend Reference to the code-generation engine.
     * @param intr Intrinsic declaration (for per-param widths).
     * @param N Declared return bit-width.
     * @param W Smallest native machine width fitting N (8, 16, 32, or 64).
     * @param sty Name of the signed type corresponding to W (e.g. "int32_t").
     * @param uty Name of the unsigned type corresponding to W.
     */
    virtual void emit(
        CBackend &backend, const IntrinsicDecl &intr, uint32_t N, uint32_t W,
        const std::string &sty, const std::string &uty
    ) const = 0;

  protected:
    static std::ostream &out(CBackend &backend);

    static uint32_t widen(uint32_t N) {
      return (N <= 8) ? 8 : (N <= 16) ? 16 : (N <= 32) ? 32 : 64;
    }

    static std::string sIntTy(uint32_t W) { return "int" + std::to_string(W) + "_t"; }

    static std::string uIntTy(uint32_t W) { return "uint" + std::to_string(W) + "_t"; }

    /**
     * @brief Parameter declared bit-width (panics if non-integer, which
     * cannot happen for v0.2.2 extra batch A&B intrinsics: semchecker enforces integer
     * parameters).
     */
    static uint32_t paramN(const IntrinsicDecl &intr, size_t i) {
      auto pb = TypeUtils::getIntBitWidth(intr.params[i].type);
      return pb ? *pb : 32;
    }

    /**
     * @brief Sign-extend an expression to occupy `W` bits, treating its low
     * `N` bits as a signed `iN` value: (sty)((uty)(expr) << (W − N)) >> (W − N).
     */
    static std::string makeSextN(
        uint32_t N, uint32_t W, const std::string &sty, const std::string &uty,
        const std::string &expr
    ) {
      if (N == W)
        return expr;
      return "(" + sty + ")((" + uty + ")(" + expr + ") << " + std::to_string(W - N) + ") >> " +
             std::to_string(W - N);
    }

    /**
     * @brief Unsigned bitmask expression that clears bits above bit `N − 1`.
     */
    static std::string
    makeMaskU(uint32_t N, uint32_t W, const std::string &uty, const std::string &expr) {
      if (N == W)
        return "(" + uty + ")(" + expr + ")";
      return "((" + uty + ")(" + expr + ") & ((" + uty + ")1 << " + std::to_string(N) + ") - 1)";
    }
  };

  /**
   * @brief Friendship-mediator registry to fetch private members of CBackend.
   */
  struct CIntrinsicRegistry {
    static std::ostream &out(CBackend &backend) { return backend.out_; }

    using CIntrinsicGenFn = std::unique_ptr<class CIntrinsic>;

    static const std::unordered_map<IntrinsicKind, CIntrinsicGenFn> &getRegistry();
  };

  std::ostream &CIntrinsic::out(CBackend &backend) { return CIntrinsicRegistry::out(backend); }

  namespace {

    // ── §12.1 Arithmetic intrinsics ─────────────────────────────────────────

    class AbsIntrinsic final : public CIntrinsic {
    public:
      void emit(
          CBackend &backend, const IntrinsicDecl &, uint32_t N, uint32_t W, const std::string &sty,
          const std::string &uty
      ) const override {
        int64_t int_min_N = (N == 64) ? INT64_MIN : -(INT64_C(1) << (N - 1));
        out(backend) << "  if (a0 == (" << sty << ")" << int_min_N << "LL) __builtin_trap();\n";
        if (W <= 32)
          out(backend) << "  " << sty << " r = (" << sty << ")abs((int)a0);\n";
        else
          out(backend) << "  " << sty << " r = (" << sty << ")llabs((long long)a0);\n";
        out(backend) << "  return " << makeSextN(N, W, sty, uty, "r") << ";\n";
      }
    };

    class MinIntrinsic final : public CIntrinsic {
    public:
      void emit(
          CBackend &backend, const IntrinsicDecl &, uint32_t N, uint32_t W, const std::string &sty,
          const std::string &uty
      ) const override {
        out(backend) << "  " << sty << " r = a0 < a1 ? a0 : a1;\n";
        out(backend) << "  return " << makeSextN(N, W, sty, uty, "r") << ";\n";
      }
    };

    class MaxIntrinsic final : public CIntrinsic {
    public:
      void emit(
          CBackend &backend, const IntrinsicDecl &, uint32_t N, uint32_t W, const std::string &sty,
          const std::string &uty
      ) const override {
        out(backend) << "  " << sty << " r = a0 > a1 ? a0 : a1;\n";
        out(backend) << "  return " << makeSextN(N, W, sty, uty, "r") << ";\n";
      }
    };

    // ── §12.2 Bit-counting intrinsics ───────────────────────────────────────

    class PopcountIntrinsic final : public CIntrinsic {
    public:
      void emit(
          CBackend &backend, const IntrinsicDecl &, uint32_t N, uint32_t W, const std::string &sty,
          const std::string &uty
      ) const override {
        out(backend) << "  " << uty << " u = " << makeMaskU(N, W, uty, "a0") << ";\n";
        if (W <= 32)
          out(backend) << "  " << sty << " r = (" << sty << ")__builtin_popcount(u);\n";
        else
          out(backend) << "  " << sty << " r = (" << sty << ")__builtin_popcountll((uint64_t)u);\n";
        int64_t maxN = (N == 64) ? INT64_MAX : ((INT64_C(1) << (N - 1)) - 1);
        out(backend) << "  if (r > " << maxN << "LL) __builtin_trap();\n";
        out(backend) << "  return " << makeSextN(N, W, sty, uty, "r") << ";\n";
      }
    };

    class ClzIntrinsic final : public CIntrinsic {
    public:
      void emit(
          CBackend &backend, const IntrinsicDecl &, uint32_t N, uint32_t W, const std::string &sty,
          const std::string &uty
      ) const override {
        out(backend) << "  " << uty << " u = " << makeMaskU(N, W, uty, "a0") << ";\n";
        out(backend) << "  if (u == 0) __builtin_trap();\n";
        if (W <= 32) {
          out(backend) << "  " << sty << " r = (" << sty << ")__builtin_clz((uint32_t)u) - "
                       << std::to_string(32 - N) << ";\n";
        } else {
          out(backend) << "  " << sty << " r = (" << sty << ")__builtin_clzll((uint64_t)u) - "
                       << std::to_string(64 - N) << ";\n";
        }
        out(backend) << "  return " << makeSextN(N, W, sty, uty, "r") << ";\n";
      }
    };

    class CtzIntrinsic final : public CIntrinsic {
    public:
      void emit(
          CBackend &backend, const IntrinsicDecl &, uint32_t N, uint32_t W, const std::string &sty,
          const std::string &uty
      ) const override {
        out(backend) << "  " << uty << " u = " << makeMaskU(N, W, uty, "a0") << ";\n";
        out(backend) << "  if (u == 0) __builtin_trap();\n";
        if (W <= 32)
          out(backend) << "  " << sty << " r = (" << sty << ")__builtin_ctz((uint32_t)u);\n";
        else
          out(backend) << "  " << sty << " r = (" << sty << ")__builtin_ctzll((uint64_t)u);\n";
        out(backend) << "  return " << makeSextN(N, W, sty, uty, "r") << ";\n";
      }
    };

    // ── §12.3 Integer extras (v0.2.2 extra batch A) ────────────────────────────────

    class AbsDiffIntrinsic final : public CIntrinsic {
    public:
      void emit(
          CBackend &backend, const IntrinsicDecl &, uint32_t N, uint32_t W, const std::string &sty,
          const std::string &uty
      ) const override {
        // Compute the subtraction in a strictly wider signed type so signed
        // overflow cannot occur, then check |s| fits in signed iN. No
        // reinterpretation as unsigned — SymIR has no unsigned types.
        int64_t maxN = (N == 64) ? INT64_MAX : ((INT64_C(1) << (N - 1)) - 1);
        if (N < 64) {
          out(backend) << "  int64_t s = (int64_t)a0 - (int64_t)a1;\n";
          out(backend) << "  int64_t r = s < 0 ? -s : s;\n";
          out(backend) << "  if (r > (int64_t)" << maxN << "LL) __builtin_trap();\n";
          out(backend) << "  return " << makeSextN(N, W, sty, uty, "(" + sty + ")r") << ";\n";
        } else {
          // N == 64: subtraction would overflow i64; use __int128.
          out(backend) << "  __int128 s = (__int128)a0 - (__int128)a1;\n";
          out(backend) << "  __int128 r = s < 0 ? -s : s;\n";
          out(backend) << "  if (r > (__int128)" << maxN << "LL) __builtin_trap();\n";
          out(backend) << "  return (" << sty << ")r;\n";
        }
      }
    };

    class SignumIntrinsic final : public CIntrinsic {
    public:
      void emit(
          CBackend &backend, const IntrinsicDecl &, uint32_t N, uint32_t W, const std::string &sty,
          const std::string &uty
      ) const override {
        // Predicate: return type is i1 (N=1, W=8). SymIR's i1 convention
        // stores the boolean as the literal 0 or 1 (matching `cmp` lowering),
        // not sign-extended — so no final sextN is applied.
        if (N == 1) {
          (void) W;
          (void) uty;
          out(backend) << "  return (" << sty << ")((a0 > 0) - (a0 < 0));\n";
        } else {
          out(backend) << "  " << sty << " r = (" << sty << ")((a0 > 0) - (a0 < 0));\n";
          out(backend) << "  return " << makeSextN(N, W, sty, uty, "r") << ";\n";
        }
      }
    };

    class ClampIntrinsic final : public CIntrinsic {
    public:
      void emit(
          CBackend &backend, const IntrinsicDecl &, uint32_t N, uint32_t W, const std::string &sty,
          const std::string &uty
      ) const override {
        out(backend) << "  if (a1 > a2) __builtin_trap();\n";
        out(backend) << "  " << sty << " r = (a0 < a1) ? a1 : (a0 > a2 ? a2 : a0);\n";
        out(backend) << "  return " << makeSextN(N, W, sty, uty, "r") << ";\n";
      }
    };

    class MidpointIntrinsic final : public CIntrinsic {
    public:
      void emit(
          CBackend &backend, const IntrinsicDecl &, uint32_t N, uint32_t W, const std::string &sty,
          const std::string &uty
      ) const override {
        if (N <= 63) {
          out(backend) << "  int64_t s = (int64_t)a0 + (int64_t)a1;\n";
          out(backend) << "  " << sty << " r = (" << sty << ")(s / 2);\n";
          out(backend) << "  return " << makeSextN(N, W, sty, uty, "r") << ";\n";
        } else {
          // i64 midpoint via __int128 (GCC/Clang only — required by SymIR
          // toolchain in this build).
          out(backend) << "  __int128 s = (__int128)a0 + (__int128)a1;\n";
          out(backend) << "  " << sty << " r = (" << sty << ")(s / 2);\n";
          out(backend) << "  return " << makeSextN(N, W, sty, uty, "r") << ";\n";
        }
      }
    };

    // ── §12.4 Bit-manipulation (v0.2.2 extra batch B) ──────────────────────────────

    class ParityIntrinsic final : public CIntrinsic {
    public:
      void emit(
          CBackend &backend, const IntrinsicDecl &intr, uint32_t N, uint32_t W,
          const std::string &sty, const std::string &uty
      ) const override {
        // Predicate: return type is i1 (N=1, W=8). Read the input at the
        // *parameter's* own width, not the return's. SymIR's i1 convention
        // stores the boolean as the literal 0 or 1 (matching `cmp` lowering),
        // not sign-extended — so no final sextN is applied.
        (void) N;
        (void) W;
        (void) uty;
        uint32_t pN = paramN(intr, 0);
        uint32_t pW = widen(pN);
        std::string puty = uIntTy(pW);
        out(backend) << "  " << puty << " u = " << makeMaskU(pN, pW, puty, "a0") << ";\n";
        if (pW <= 32)
          out(backend) << "  return (" << sty << ")(__builtin_parity(u) & 1);\n";
        else
          out(backend) << "  return (" << sty << ")(__builtin_parityll((uint64_t)u) & 1);\n";
      }
    };

    class BswapIntrinsic final : public CIntrinsic {
    public:
      void emit(
          CBackend &backend, const IntrinsicDecl &, uint32_t N, uint32_t W, const std::string &sty,
          const std::string &uty
      ) const override {
        // Generic byte-reverse loop. Compilers fold this for power-of-two
        // widths; for non-power-of-two byte counts (i24, i40, …) it remains a
        // short inline loop.
        out(backend) << "  " << uty << " u = " << makeMaskU(N, W, uty, "a0") << ";\n";
        out(backend) << "  " << uty << " r = 0;\n";
        out(backend) << "  for (uint32_t i = 0; i < " << (N / 8) << "; ++i) {\n";
        out(backend) << "    r |= ((u >> (i*8)) & 0xFF) << ((" << (N / 8 - 1) << " - i) * 8);\n";
        out(backend) << "  }\n";
        out(backend) << "  return " << makeSextN(N, W, sty, uty, "(" + sty + ")r") << ";\n";
      }
    };

    class BitreverseIntrinsic final : public CIntrinsic {
    public:
      void emit(
          CBackend &backend, const IntrinsicDecl &, uint32_t N, uint32_t W, const std::string &sty,
          const std::string &uty
      ) const override {
        out(backend) << "  " << uty << " u = " << makeMaskU(N, W, uty, "a0") << ";\n";
        out(backend) << "  " << uty << " r = 0;\n";
        out(backend) << "  for (uint32_t i = 0; i < " << N << "; ++i) {\n";
        out(backend) << "    if ((u >> i) & 1) r |= ((" << uty << ")1 << (" << (N - 1)
                     << " - i));\n";
        out(backend) << "  }\n";
        out(backend) << "  return " << makeSextN(N, W, sty, uty, "(" + sty + ")r") << ";\n";
      }
    };

    class RotlIntrinsic final : public CIntrinsic {
    public:
      void emit(
          CBackend &backend, const IntrinsicDecl &, uint32_t N, uint32_t W, const std::string &sty,
          const std::string &uty
      ) const override {
        out(backend) << "  if (a1 < 0 || a1 >= " << N << ") __builtin_trap();\n";
        out(backend) << "  " << uty << " u = " << makeMaskU(N, W, uty, "a0") << ";\n";
        out(backend) << "  " << uty << " n = (" << uty << ")a1;\n";
        out(backend) << "  " << uty << " r = (n == 0) ? u : ((u << n) | (u >> (" << N
                     << " - n)));\n";
        if (N < W)
          out(backend) << "  r &= ((" << uty << ")1 << " << N << ") - 1;\n";
        out(backend) << "  return " << makeSextN(N, W, sty, uty, "(" + sty + ")r") << ";\n";
      }
    };

    class RotrIntrinsic final : public CIntrinsic {
    public:
      void emit(
          CBackend &backend, const IntrinsicDecl &, uint32_t N, uint32_t W, const std::string &sty,
          const std::string &uty
      ) const override {
        out(backend) << "  if (a1 < 0 || a1 >= " << N << ") __builtin_trap();\n";
        out(backend) << "  " << uty << " u = " << makeMaskU(N, W, uty, "a0") << ";\n";
        out(backend) << "  " << uty << " n = (" << uty << ")a1;\n";
        out(backend) << "  " << uty << " r = (n == 0) ? u : ((u >> n) | (u << (" << N
                     << " - n)));\n";
        if (N < W)
          out(backend) << "  r &= ((" << uty << ")1 << " << N << ") - 1;\n";
        out(backend) << "  return " << makeSextN(N, W, sty, uty, "(" + sty + ")r") << ";\n";
      }
    };

    class IsPow2Intrinsic final : public CIntrinsic {
    public:
      void emit(
          CBackend &backend, const IntrinsicDecl &intr, uint32_t N, uint32_t W,
          const std::string &sty, const std::string &uty
      ) const override {
        // Predicate: return i1 stored as literal 0 / 1 (SymIR convention,
        // matches `cmp` lowering). No final sextN.
        (void) N;
        (void) W;
        (void) uty;
        uint32_t pN = paramN(intr, 0);
        uint32_t pW = widen(pN);
        std::string psty = sIntTy(pW);
        (void) pN;
        out(backend) << "  return (" << sty << ")((a0 > 0) && (((" << psty << ")a0 & ((" << psty
                     << ")a0 - 1)) == 0));\n";
      }
    };

    class Ilog2Intrinsic final : public CIntrinsic {
    public:
      void emit(
          CBackend &backend, const IntrinsicDecl &, uint32_t N, uint32_t W, const std::string &sty,
          const std::string &uty
      ) const override {
        out(backend) << "  if (a0 <= 0) __builtin_trap();\n";
        out(backend) << "  " << uty << " u = " << makeMaskU(N, W, uty, "a0") << ";\n";
        if (W <= 32)
          out(backend) << "  " << sty << " r = (" << sty << ")(" << (N - 1)
                       << " - __builtin_clz((uint32_t)u) + " << (32 - N) << ");\n";
        else
          out(backend) << "  " << sty << " r = (" << sty << ")(" << (N - 1)
                       << " - __builtin_clzll((uint64_t)u) + " << (64 - N) << ");\n";
        out(backend) << "  return " << makeSextN(N, W, sty, uty, "r") << ";\n";
      }
    };

  } // namespace

  const std::unordered_map<IntrinsicKind, CIntrinsicRegistry::CIntrinsicGenFn> &
  CIntrinsicRegistry::getRegistry() {
    static const std::unordered_map<IntrinsicKind, CIntrinsicGenFn> registry = []() {
      std::unordered_map<IntrinsicKind, CIntrinsicGenFn> r;
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

  // ── naming ───────────────────────────────────────────────────────────────
  std::string CBackend::intrinsicHelperName(const std::string &intrName, uint32_t bits) const {
    std::string base = intrName;
    if (!base.empty() && base[0] == '@')
      base.erase(0, 1);
    return "_symir_" + base + "_i" + std::to_string(bits);
  }

  // ── emission ─────────────────────────────────────────────────────────────
  void CBackend::emitIntrinsicHelper(const IntrinsicDecl &intr) {
    auto rb = TypeUtils::getIntBitWidth(intr.retType);
    if (!rb)
      return; // non-integer intrinsics aren't supported in v0.2.2
    uint32_t N = *rb;
    uint32_t W = (N <= 8) ? 8 : (N <= 16) ? 16 : (N <= 32) ? 32 : 64;
    std::string sty = "int" + std::to_string(W) + "_t";
    std::string uty = "uint" + std::to_string(W) + "_t";
    std::string outTy = sty;
    std::string name = intrinsicHelperName(intr.name.name, N);

    out_ << "static inline " << outTy << " " << name << "(";
    for (size_t i = 0; i < intr.params.size(); ++i) {
      if (i)
        out_ << ", ";
      // Per-param widening so predicate intrinsics (i1 return, iN input)
      // get the right argument type in the helper signature.
      uint32_t pN = 32;
      if (auto pb = TypeUtils::getIntBitWidth(intr.params[i].type))
        pN = *pb;
      uint32_t pW = (pN <= 8) ? 8 : (pN <= 16) ? 16 : (pN <= 32) ? 32 : 64;
      out_ << "int" << pW << "_t a" << i;
    }
    out_ << ") {\n";

    auto kind = getIntrinsicKind(intr.name.name);
    if (kind) {
      const auto &registry = CIntrinsicRegistry::getRegistry();
      auto it = registry.find(*kind);
      if (it != registry.end()) {
        it->second->emit(*this, intr, N, W, sty, uty);
        out_ << "}\n\n";
        return;
      }
    }

    out_ << "  __builtin_trap(); /* unknown intrinsic */\n";
    out_ << "}\n\n";
  }

} // namespace symir
