// [v0.2.2] WASM backend intrinsic helper emission.
//
// This file is the single source of truth for the WebAssembly Text Format
// (WAT) code emitted for every built-in SymIR intrinsic. The helpers use
// a widening-and-mask strategy: each iN operation is widened to the next
// WASM native width (i32 or i64), performed there, then sign-masked back
// to N bits. UB-preconditions abort via `unreachable`.
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

  /**
   * @brief Abstract base class for WASM Text Format (WAT) code-generation of intrinsics.
   * Subclasses generate WAT statements inside helper function blocks.
   */
  class WasmIntrinsic {
  public:
    virtual ~WasmIntrinsic() = default;

    /**
     * @brief Generates WAT statements for the helper function body.
     * @param backend Reference to the code-generation engine.
     * @param N Declared bit-width of the intrinsic.
     * @param W Smallest native WASM width fitting N (32 or 64).
     * @param ity Native WASM type name ("i32" or "i64").
     */
    virtual void
    emit(WasmBackend &backend, uint32_t N, uint32_t W, const std::string &ity) const = 0;

  protected:
    /**
     * @brief Helpers to route backend stream access through WasmIntrinsicRegistry.
     */
    static std::ostream &out(WasmBackend &backend);

    static void indent(WasmBackend &backend);

    static void incrIndent(WasmBackend &backend);

    static void decrIndent(WasmBackend &backend);

    /**
     * @brief Generates local.get instructions to load an argument onto the WASM stack.
     */
    static void pushArg(WasmBackend &backend, size_t i) {
      indent(backend);
      out(backend) << "local.get $a" << i << "\n";
    }

    /**
     * @brief Generates WASM instructions to sign-extend the top-of-stack back to N bits.
     * Shifts left by W-N bits, then performs signed right shift by W-N bits:
     *   (ity).const (W - N) -> (ity).shl -> (ity).const (W - N) -> (ity).shr_s
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
     * @brief Generates a mask value on the WASM stack.
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
  };

  /**
   * @brief Friendship-mediator registry to fetch private members of WasmBackend.
   * This class is declared as a friend of WasmBackend, allowing subclasses of
   * WasmIntrinsic (which are located in the anonymous namespace) to access WasmBackend's
   * private stream `out_`, private indentation helpers, and variables.
   */
  struct WasmIntrinsicRegistry {
    /**
     * @brief Exposes the output stream of the given WasmBackend.
     */
    static std::ostream &out(WasmBackend &backend) { return backend.out_; }

    /**
     * @brief Writes spaces representing current indent level to the stream.
     */
    static void indent(WasmBackend &backend) { backend.indent(); }

    /**
     * @brief Increments the indentation level.
     */
    static void incrIndent(WasmBackend &backend) { backend.indent_level_++; }

    /**
     * @brief Decrements the indentation level.
     */
    static void decrIndent(WasmBackend &backend) { backend.indent_level_--; }

    using WasmIntrinsicGenFn = std::unique_ptr<WasmIntrinsic>;

    /**
     * @brief Singleton registry getter for all supported WasmIntrinsic generators.
     */
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

    /**
     * @brief WASM emission for the @abs(x) intrinsic.
     * Traps (unreachable) if the input is INT_MIN_N.
     * Computes the absolute value by selecting between 0-a0 and a0 based on lt_s.
     */
    class AbsIntrinsic final : public WasmIntrinsic {
    public:
      void
      emit(WasmBackend &backend, uint32_t N, uint32_t W, const std::string &ity) const override {
        int64_t int_min_N = (N == 64) ? INT64_MIN : -(INT64_C(1) << (N - 1));
        pushArg(backend, 0);
        indent(backend);
        out(backend) << ity << ".const " << int_min_N << "\n";
        indent(backend);
        out(backend) << ity << ".eq\n";
        indent(backend);
        out(backend) << "if\n";
        incrIndent(backend);
        indent(backend);
        out(backend) << "unreachable\n";
        decrIndent(backend);
        indent(backend);
        out(backend) << "end\n";

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

    /**
     * @brief WASM emission for the @min(a, b) intrinsic.
     * Computes signed minimum using the select instruction and lt_s comparison.
     */
    class MinIntrinsic final : public WasmIntrinsic {
    public:
      void
      emit(WasmBackend &backend, uint32_t N, uint32_t W, const std::string &ity) const override {
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

    /**
     * @brief WASM emission for the @max(a, b) intrinsic.
     * Computes signed maximum using the select instruction and gt_s comparison.
     */
    class MaxIntrinsic final : public WasmIntrinsic {
    public:
      void
      emit(WasmBackend &backend, uint32_t N, uint32_t W, const std::string &ity) const override {
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

    /**
     * @brief WASM emission for the @popcount(x) intrinsic.
     * Computes popcount using WASM's native popcnt instruction on a masked input.
     */
    class PopcountIntrinsic final : public WasmIntrinsic {
    public:
      void
      emit(WasmBackend &backend, uint32_t N, uint32_t W, const std::string &ity) const override {
        pushArg(backend, 0);
        pushMask(backend, N, W, ity);
        indent(backend);
        out(backend) << ity << ".and\n";
        indent(backend);
        out(backend) << ity << ".popcnt\n";
        sextN(backend, N, W, ity);
      }
    };

    /**
     * @brief WASM emission for the @clz(x) intrinsic.
     * Computes leading zeros using WASM's native clz instruction.
     * Traps (unreachable) if the masked input is zero.
     * Subtracts the W-N bit bias to exclude high bits masked off.
     */
    class ClzIntrinsic final : public WasmIntrinsic {
    public:
      void
      emit(WasmBackend &backend, uint32_t N, uint32_t W, const std::string &ity) const override {
        pushArg(backend, 0);
        pushMask(backend, N, W, ity);
        indent(backend);
        out(backend) << ity << ".and\n";
        indent(backend);
        out(backend) << "local.tee $tmp0\n";
        indent(backend);
        out(backend) << ity << ".eqz\n";
        indent(backend);
        out(backend) << "if\n";
        incrIndent(backend);
        indent(backend);
        out(backend) << "unreachable\n";
        decrIndent(backend);
        indent(backend);
        out(backend) << "end\n";
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

    /**
     * @brief WASM emission for the @ctz(x) intrinsic.
     * Computes trailing zeros using WASM's native ctz instruction.
     * Traps (unreachable) if the masked input is zero.
     */
    class CtzIntrinsic final : public WasmIntrinsic {
    public:
      void
      emit(WasmBackend &backend, uint32_t N, uint32_t W, const std::string &ity) const override {
        pushArg(backend, 0);
        pushMask(backend, N, W, ity);
        indent(backend);
        out(backend) << ity << ".and\n";
        indent(backend);
        out(backend) << "local.tee $tmp0\n";
        indent(backend);
        out(backend) << ity << ".eqz\n";
        indent(backend);
        out(backend) << "if\n";
        incrIndent(backend);
        indent(backend);
        out(backend) << "unreachable\n";
        decrIndent(backend);
        indent(backend);
        out(backend) << "end\n";
        indent(backend);
        out(backend) << "local.get $tmp0\n";
        indent(backend);
        out(backend) << ity << ".ctz\n";
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
      out_ << " (param $a" << i << " " << ity << ")";
    }
    out_ << " (result " << ity << ")\n";
    indent_level_++;

    // Declare a scratch local for @clz/@ctz (UB-check on zero).
    const std::string &intrN = intr.name.name;
    if (intrN == "@clz" || intrN == "@ctz") {
      indent();
      out_ << "(local $tmp0 " << ity << ")\n";
    }

    auto kind = getIntrinsicKind(intr.name.name);
    if (kind) {
      const auto &registry = WasmIntrinsicRegistry::getRegistry();
      auto it = registry.find(*kind);
      if (it != registry.end()) {
        it->second->emit(*this, N, W, ity);
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
