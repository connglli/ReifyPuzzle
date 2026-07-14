#pragma once

#include "ast/ast.hpp"

namespace refractir {

  // Python float-literal formatter. Unlike the C/WASM backends (whose
  // grammars need suffixes / infinity syntax and carry their own
  // formatters), Python's decimal float grammar is exactly the
  // canonical refractir::formatDouble output: shortest round-trip
  // decimal with a guaranteed '.'/exponent, parsed correctly-rounded
  // to IEEE double by the interpreter. So the python backend uses the
  // canonical formatter directly — no intentional divergence here.
  inline std::string formatFloatLit(double v) { return formatDouble(v); }

} // namespace refractir
