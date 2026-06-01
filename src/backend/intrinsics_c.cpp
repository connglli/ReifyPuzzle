// [v0.2.2] C backend intrinsic helper emission.
//
// This file is the single source of truth for the C code emitted for
// every built-in SymIR intrinsic. The helpers use a widening-and-mask
// strategy: each iN operation is widened to the next machine integer
// width (8/16/32/64), performed there, then sign-masked back to N bits.
// UB-preconditions abort via __builtin_trap.
//
// To add a new intrinsic:
//   1. Add its IntrinsicKind to include/analysis/intrinsics.hpp.
//   2. Implement a subclass of CIntrinsic and register it below.

#include "backend/c_backend.hpp"

#include <cstdint>
#include <functional>
#include <memory>
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
     * @param N Declared bit-width of the intrinsic.
     * @param W Smallest native machine width fitting N (8, 16, 32, or 64).
     * @param sty Name of the signed type corresponding to width W (e.g., "int32_t").
     * @param uty Name of the unsigned type corresponding to width W (e.g., "uint32_t").
     */
    virtual void emit(
        CBackend &backend, uint32_t N, uint32_t W, const std::string &sty, const std::string &uty
    ) const = 0;

  protected:
    /**
     * @brief Helper to fetch the C backend's private stream via CIntrinsicRegistry.
     */
    static std::ostream &out(CBackend &backend);

    /**
     * @brief Generates a sign-extension expression back to N bits in C.
     * Uses arithmetic right shift for sign extension: (sty)((uty)(expr) << (W - N)) >> (W - N).
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
     * @brief Generates an unsigned bitmask expression to clear bits above N-1.
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
   * This class is declared as a friend of CBackend, allowing subclasses of
   * CIntrinsic (which are located in the anonymous namespace) to access CBackend's
   * private stream `out_` safely.
   */
  struct CIntrinsicRegistry {
    /**
     * @brief Exposes the output stream of the given CBackend.
     */
    static std::ostream &out(CBackend &backend) { return backend.out_; }

    using CIntrinsicGenFn = std::unique_ptr<class CIntrinsic>;

    /**
     * @brief Singleton registry getter for all supported CIntrinsic generators.
     */
    static const std::unordered_map<IntrinsicKind, CIntrinsicGenFn> &getRegistry();
  };

  std::ostream &CIntrinsic::out(CBackend &backend) { return CIntrinsicRegistry::out(backend); }

  namespace {

    /**
     * @brief C emission for the @abs(x) intrinsic.
     * Generates a sign-extension ternary branch-free operation: x < 0 ? -x : x.
     * Inserts an assertion check that traps if input is INT_MIN_N.
     */
    class AbsIntrinsic final : public CIntrinsic {
    public:
      void emit(
          CBackend &backend, uint32_t N, uint32_t W, const std::string &sty, const std::string &uty
      ) const override {
        int64_t int_min_N = (N == 64) ? INT64_MIN : -(INT64_C(1) << (N - 1));
        out(backend) << "  if (a0 == (" << sty << ")" << int_min_N << "LL) __builtin_trap();\n";
        out(backend) << "  " << sty << " r = a0 < 0 ? -a0 : a0;\n";
        out(backend) << "  return " << makeSextN(N, W, sty, uty, "r") << ";\n";
      }
    };

    /**
     * @brief C emission for the @min(a, b) intrinsic.
     * Emits a simple ternary condition: a < b ? a : b.
     */
    class MinIntrinsic final : public CIntrinsic {
    public:
      void emit(
          CBackend &backend, uint32_t N, uint32_t W, const std::string &sty, const std::string &uty
      ) const override {
        out(backend) << "  " << sty << " r = a0 < a1 ? a0 : a1;\n";
        out(backend) << "  return " << makeSextN(N, W, sty, uty, "r") << ";\n";
      }
    };

    /**
     * @brief C emission for the @max(a, b) intrinsic.
     * Emits a simple ternary condition: a > b ? a : b.
     */
    class MaxIntrinsic final : public CIntrinsic {
    public:
      void emit(
          CBackend &backend, uint32_t N, uint32_t W, const std::string &sty, const std::string &uty
      ) const override {
        out(backend) << "  " << sty << " r = a0 > a1 ? a0 : a1;\n";
        out(backend) << "  return " << makeSextN(N, W, sty, uty, "r") << ";\n";
      }
    };

    /**
     * @brief C emission for the @popcount(x) intrinsic.
     * Emits Clang/GCC __builtin_popcount or __builtin_popcountll depending on width W.
     * Widens the operand, applies an unsigned mask, counts bits, and sign-masks the result.
     */
    class PopcountIntrinsic final : public CIntrinsic {
    public:
      void emit(
          CBackend &backend, uint32_t N, uint32_t W, const std::string &sty, const std::string &uty
      ) const override {
        out(backend) << "  " << uty << " u = " << makeMaskU(N, W, uty, "a0") << ";\n";
        if (W <= 32)
          out(backend) << "  " << sty << " r = (" << sty << ")__builtin_popcount(u);\n";
        else
          out(backend) << "  " << sty << " r = (" << sty << ")__builtin_popcountll((uint64_t)u);\n";
        out(backend) << "  return " << makeSextN(N, W, sty, uty, "r") << ";\n";
      }
    };

    /**
     * @brief C emission for the @clz(x) intrinsic.
     * Emits Clang/GCC __builtin_clz or __builtin_clzll.
     * Pre-masks the value and checks for zero (trapping if true).
     * Adjusts the builtin result by subtracting the bias (32-N or 64-N)
     * because the builtin counts zeros in the full machine word W.
     */
    class ClzIntrinsic final : public CIntrinsic {
    public:
      void emit(
          CBackend &backend, uint32_t N, uint32_t W, const std::string &sty, const std::string &uty
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

    /**
     * @brief C emission for the @ctz(x) intrinsic.
     * Emits Clang/GCC __builtin_ctz or __builtin_ctzll.
     * Checks for zero (trapping if true).
     */
    class CtzIntrinsic final : public CIntrinsic {
    public:
      void emit(
          CBackend &backend, uint32_t N, uint32_t W, const std::string &sty, const std::string &uty
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
    std::string outTy = "int" + std::to_string(W) + "_t"; // matches emitType for iN
    std::string name = intrinsicHelperName(intr.name.name, N);

    out_ << "static inline " << outTy << " " << name << "(";
    for (size_t i = 0; i < intr.params.size(); ++i) {
      if (i)
        out_ << ", ";
      out_ << sty << " a" << i;
    }
    out_ << ") {\n";

    auto kind = getIntrinsicKind(intr.name.name);
    if (kind) {
      const auto &registry = CIntrinsicRegistry::getRegistry();
      auto it = registry.find(*kind);
      if (it != registry.end()) {
        it->second->emit(*this, N, W, sty, uty);
        out_ << "}\n\n";
        return;
      }
    }

    out_ << "  __builtin_trap(); /* unknown intrinsic */\n";
    out_ << "}\n\n";
  }

} // namespace symir
