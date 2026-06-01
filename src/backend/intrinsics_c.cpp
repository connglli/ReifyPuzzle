// [v0.2.2] C backend intrinsic helper emission.
//
// This file is the single source of truth for the C code emitted for
// every built-in SymIR intrinsic. The helpers use a widening-and-mask
// strategy: each iN operation is widened to the next machine integer
// width (8/16/32/64), performed there, then sign-masked back to N bits.
// UB-preconditions abort via __builtin_trap.
//
// To add a new intrinsic:
//   1. Add its `intrinsic @name(...)` declaration to the test / user
//      program.
//   2. Add a branch in CBackend::emitIntrinsicHelper below.
//   3. Add the matching branches in:
//        src/interp/intrinsics.cpp        (interpreter concrete evaluation)
//        src/solver/intrinsics.cpp        (SMT lowering)
//        src/backend/intrinsics_wasm.cpp  (WASM code-gen helper)
//
// Currently supported (§12):
//   @abs(x)      – branch-free abs via negation; UB trap on INT_MIN_N
//   @min(a, b)   – ternary conditional
//   @max(a, b)   – ternary conditional
//   @popcount(x) – __builtin_popcount / __builtin_popcountll
//   @clz(x)      – __builtin_clz / __builtin_clzll with bias; UB trap on 0
//   @ctz(x)      – __builtin_ctz / __builtin_ctzll; UB trap on 0

#include "backend/c_backend.hpp"

#include <cstdint>
#include "analysis/type_utils.hpp"

namespace symir {

  // ── naming ───────────────────────────────────────────────────────────────
  std::string CBackend::intrinsicHelperName(const std::string &intrName, uint32_t bits) const {
    // intrName begins with '@'; drop it.
    std::string base = intrName;
    if (!base.empty() && base[0] == '@')
      base.erase(0, 1);
    return "_symir_" + base + "_i" + std::to_string(bits);
  }

  // ── emission ─────────────────────────────────────────────────────────────
  // [v0.2.2] §11.5 widening-and-mask lowering. We emit one static inline
  // helper per (intrinsic, bit-width). Each helper widens to the next
  // larger machine width, performs the operation, then sign-extends /
  // masks the result back to N bits. UB-preconditions abort via
  // __builtin_trap (matches the `assert`-on-violation pattern other UB
  // sites use).
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

    // Sign-mask helper: arithmetic-shift back to N bits.
    auto sextN = [&](const std::string &expr) -> std::string {
      if (N == W)
        return expr;
      return "(" + sty + ")((" + uty + ")(" + expr + ") << " + std::to_string(W - N) + ") >> " +
             std::to_string(W - N);
    };
    // Unsigned mask to N bits: zero out the high (W-N) bits.
    auto maskU = [&](const std::string &expr) -> std::string {
      if (N == W)
        return "(" + uty + ")(" + expr + ")";
      return "((" + uty + ")(" + expr + ") & ((" + uty + ")1 << " + std::to_string(N) + ") - 1)";
    };

    const std::string &n = intr.name.name;

    // ── @abs ───────────────────────────────────────────────────────────────
    if (n == "@abs") {
      int64_t int_min_N = (N == 64) ? INT64_MIN : -(INT64_C(1) << (N - 1));
      out_ << "  if (a0 == (" << sty << ")" << int_min_N << "LL) __builtin_trap();\n";
      out_ << "  " << sty << " r = a0 < 0 ? -a0 : a0;\n";
      out_ << "  return " << sextN("r") << ";\n";

      // ── @min ───────────────────────────────────────────────────────────────
    } else if (n == "@min") {
      out_ << "  " << sty << " r = a0 < a1 ? a0 : a1;\n";
      out_ << "  return " << sextN("r") << ";\n";

      // ── @max ───────────────────────────────────────────────────────────────
    } else if (n == "@max") {
      out_ << "  " << sty << " r = a0 > a1 ? a0 : a1;\n";
      out_ << "  return " << sextN("r") << ";\n";

      // ── @popcount ──────────────────────────────────────────────────────────
    } else if (n == "@popcount") {
      out_ << "  " << uty << " u = " << maskU("a0") << ";\n";
      if (W <= 32)
        out_ << "  " << sty << " r = (" << sty << ")__builtin_popcount(u);\n";
      else
        out_ << "  " << sty << " r = (" << sty << ")__builtin_popcountll((uint64_t)u);\n";
      out_ << "  return " << sextN("r") << ";\n";

      // ── @clz ───────────────────────────────────────────────────────────────
    } else if (n == "@clz") {
      out_ << "  " << uty << " u = " << maskU("a0") << ";\n";
      out_ << "  if (u == 0) __builtin_trap();\n";
      if (W <= 32) {
        out_ << "  " << sty << " r = (" << sty << ")__builtin_clz((uint32_t)u) - "
             << std::to_string(32 - N) << ";\n";
      } else {
        out_ << "  " << sty << " r = (" << sty << ")__builtin_clzll((uint64_t)u) - "
             << std::to_string(64 - N) << ";\n";
      }
      out_ << "  return " << sextN("r") << ";\n";

      // ── @ctz ───────────────────────────────────────────────────────────────
    } else if (n == "@ctz") {
      out_ << "  " << uty << " u = " << maskU("a0") << ";\n";
      out_ << "  if (u == 0) __builtin_trap();\n";
      if (W <= 32)
        out_ << "  " << sty << " r = (" << sty << ")__builtin_ctz((uint32_t)u);\n";
      else
        out_ << "  " << sty << " r = (" << sty << ")__builtin_ctzll((uint64_t)u);\n";
      out_ << "  return " << sextN("r") << ";\n";

      // ── unknown (defensive) ────────────────────────────────────────────────
    } else {
      out_ << "  __builtin_trap(); /* unknown intrinsic */\n";
    }
    out_ << "}\n\n";
  }

} // namespace symir
