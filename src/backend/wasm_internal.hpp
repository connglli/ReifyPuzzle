#pragma once

// WASM-backend-private helpers shared across the backend's translation units.
// Kept inline in this detail header so each WASM-backend TU that needs them can
// pull them in without an extra object file. Not part of the public
// WasmBackend interface — internal to src/backend.

#include <iomanip>
#include <limits>
#include <sstream>
#include <string>

namespace refractir {

  // Format a double literal for WAT output. RefractIR's shared formatter
  // refractir::formatDouble (ast.hpp) emits SIR-shaped strings, not WAT — the
  // WAT float grammar has its own rules — so this stays a separate formatter.
  inline std::string formatFloatLit(double v) {
    std::ostringstream os;
    os << std::setprecision(std::numeric_limits<double>::max_digits10) << v;
    std::string s = os.str();
    if (s.find_first_of(".eEnN") == std::string::npos)
      s += ".0";
    return s;
  }

} // namespace refractir
