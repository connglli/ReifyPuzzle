// [v0.2.2] WASM backend intrinsic helper emission.
//
// This file is the single source of truth for the WebAssembly Text Format
// (WAT) code emitted for every built-in SymIR intrinsic. The helpers use
// a widening-and-mask strategy: each iN operation is widened to the next
// WASM native width (i32 or i64), performed there, then sign-masked back
// to N bits. UB-preconditions abort via `unreachable`.
//
// To add a new intrinsic:
//   1. Add its `intrinsic @name(...)` declaration to the test / user
//      program.
//   2. Add a branch in WasmBackend::emitIntrinsicHelper below.
//   3. Add the matching branches in:
//        src/interp/intrinsics.cpp        (interpreter concrete evaluation)
//        src/solver/intrinsics.cpp        (SMT lowering)
//        src/backend/intrinsics_c.cpp     (C code-gen helper)
//
// Currently supported (§12):
//   @abs(x)      – select-based abs; unreachable on INT_MIN_N
//   @min(a, b)   – select with lt_s
//   @max(a, b)   – select with gt_s
//   @popcount(x) – WASM popcnt with mask
//   @clz(x)      – WASM clz with (W-N) bias; unreachable on 0
//   @ctz(x)      – WASM ctz;                  unreachable on 0
//
// Note: float literal format intentionally diverges from symir::formatDouble.
// WAT has its own grammar rules. See AGENTS.md §FP-serialization-invariant.

#include "backend/wasm_backend.hpp"

#include <cstdint>
#include <string>
#include "analysis/type_utils.hpp"

namespace symir {

  std::string WasmBackend::intrinsicHelperName(const std::string &intrName, uint32_t bits) const {
    std::string base = intrName;
    if (!base.empty() && base[0] == '@')
      base.erase(0, 1);
    return "$_symir_" + base + "_i" + std::to_string(bits);
  }

  // [v0.2.2] §11.5 widening-and-mask. WASM has native i32/i64 popcnt/clz/ctz
  // but no abs/min/max for ints — those are emitted via select / branches.
  // The helper widens iN to i32 or i64, computes, and sign-masks back to N.
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

    auto pushArg = [&](size_t i) {
      indent();
      out_ << "local.get $a" << i << "\n";
    };
    // Mask top bits back to N (sign-extended).
    auto sextN = [&]() {
      if (N == W)
        return;
      indent();
      out_ << ity << ".const " << (W - N) << "\n";
      indent();
      out_ << ity << ".shl\n";
      indent();
      out_ << ity << ".const " << (W - N) << "\n";
      indent();
      out_ << ity << ".shr_s\n";
    };
    // Push the N-bit mask; only needed when N < W.
    auto pushMask = [&]() {
      if (N == W) {
        indent();
        out_ << ity << ".const -1\n";
      } else {
        indent();
        out_ << ity << ".const " << ((uint64_t(1) << N) - 1) << "\n";
      }
    };

    const std::string &n = intr.name.name;

    // ── @abs ───────────────────────────────────────────────────────────────
    if (n == "@abs") {
      // if (a0 == INT_MIN_N) unreachable; r = a0 < 0 ? -a0 : a0;
      int64_t int_min_N = (N == 64) ? INT64_MIN : -(INT64_C(1) << (N - 1));
      pushArg(0);
      indent();
      out_ << ity << ".const " << int_min_N << "\n";
      indent();
      out_ << ity << ".eq\n";
      indent();
      out_ << "if\n";
      indent_level_++;
      indent();
      out_ << "unreachable\n";
      indent_level_--;
      indent();
      out_ << "end\n";
      // r = a0 < 0 ? -a0 : a0  →  push -a0, a0, (a0 < 0), select
      indent();
      out_ << ity << ".const 0\n";
      pushArg(0);
      indent();
      out_ << ity << ".sub\n";
      pushArg(0);
      pushArg(0);
      indent();
      out_ << ity << ".const 0\n";
      indent();
      out_ << ity << ".lt_s\n";
      indent();
      out_ << "select\n";
      sextN();

      // ── @min / @max ────────────────────────────────────────────────────────
    } else if (n == "@min" || n == "@max") {
      bool isMin = (n == "@min");
      pushArg(0);
      pushArg(1);
      pushArg(0);
      pushArg(1);
      indent();
      out_ << ity << "." << (isMin ? "lt_s" : "gt_s") << "\n";
      indent();
      out_ << "select\n";
      sextN();

      // ── @popcount ──────────────────────────────────────────────────────────
    } else if (n == "@popcount") {
      pushArg(0);
      pushMask();
      indent();
      out_ << ity << ".and\n";
      indent();
      out_ << ity << ".popcnt\n";
      sextN();

      // ── @clz / @ctz ────────────────────────────────────────────────────────
    } else if (n == "@clz" || n == "@ctz") {
      pushArg(0);
      pushMask();
      indent();
      out_ << ity << ".and\n";
      indent();
      out_ << "local.tee $tmp0\n";
      indent();
      out_ << ity << ".eqz\n";
      indent();
      out_ << "if\n";
      indent_level_++;
      indent();
      out_ << "unreachable\n";
      indent_level_--;
      indent();
      out_ << "end\n";
      indent();
      out_ << "local.get $tmp0\n";
      indent();
      out_ << ity << "." << (n == "@clz" ? "clz" : "ctz") << "\n";
      // For clz, subtract (W-N) so leading zeros above bit N-1 aren't counted.
      if (n == "@clz" && N != W) {
        indent();
        out_ << ity << ".const " << (W - N) << "\n";
        indent();
        out_ << ity << ".sub\n";
      }
      sextN();

      // ── unknown (defensive) ────────────────────────────────────────────────
    } else {
      indent();
      out_ << "unreachable\n";
    }

    indent_level_--;
    indent();
    out_ << ")\n";
  }

} // namespace symir
