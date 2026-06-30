#pragma once

// C-backend-private helpers shared across the backend's translation units.
// Kept inline in this detail header (not a separate .cpp) so each C-backend TU
// that needs them can pull them in without an extra object file. Not part of
// the public CBackend interface — internal to src/backend.

#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include "ast/ast.hpp"

namespace refractir {

  // RAII guard for a bool flag (the C backend's isDoubleCtx_): sets it to
  // `newValue` on entry and restores the prior value on scope exit. Used at
  // every emission entry point that establishes a fresh evaluation context
  // (assign, store, return, cond, cast-from-float-lit, init).
  struct CtxGuard {
    bool &flag;
    bool saved;

    CtxGuard(bool &f, bool newValue) : flag(f), saved(f) { flag = newValue; }

    ~CtxGuard() { flag = saved; }

    CtxGuard(const CtxGuard &) = delete;
    CtxGuard &operator=(const CtxGuard &) = delete;
  };

  // Format a double literal with enough precision to round-trip exactly. The
  // C backend emits *C* literals (`.0` suffix so an integer-valued double does
  // not pick up integer type); see the note in c_backend.cpp on why this is a
  // separate formatter from refractir::formatDouble.
  inline std::string formatFloatLit(double v) {
    std::ostringstream os;
    os << std::setprecision(std::numeric_limits<double>::max_digits10) << v;
    std::string s = os.str();
    if (s.find_first_of(".eEnN") == std::string::npos)
      s += ".0";
    return s;
  }

  // True if `t` is f64 or an aggregate/pointer whose leaf is f64 (drives the
  // double-vs-float emission context).
  inline bool isOrContainsF64(const TypePtr &t) {
    if (!t)
      return false;
    return std::visit(
        [](auto &&arg) -> bool {
          using T = std::decay_t<decltype(arg)>;
          if constexpr (std::is_same_v<T, FloatType>) {
            return arg.kind == FloatType::Kind::F64;
          } else if constexpr (std::is_same_v<T, ArrayType>) {
            return isOrContainsF64(arg.elem);
          } else if constexpr (std::is_same_v<T, PtrType>) {
            return isOrContainsF64(arg.pointee);
          }
          return false;
        },
        t->v
    );
  }

  // True if `t` is the 1-bit integer type i1.
  inline bool isI1Type(const TypePtr &t) {
    if (!t)
      return false;
    return std::visit(
        [](auto &&arg) -> bool {
          using T = std::decay_t<decltype(arg)>;
          if constexpr (std::is_same_v<T, IntType>) {
            return arg.kind == IntType::Kind::ICustom && arg.bits && *arg.bits == 1;
          }
          return false;
        },
        t->v
    );
  }

} // namespace refractir
