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

  // RAII guard for PyBackend::f32Ctx_ — the float-literal evaluation
  // context, mirroring the C backend's CtxGuard/isDoubleCtx_: float
  // literals round to f32 unless an f64-typed context (assignment,
  // store, cond, ret, call arg) is active. The SPEC default for a
  // float literal is f32.
  struct F32Guard {
    bool &ref;
    bool saved;

    F32Guard(bool &r, bool v) : ref(r), saved(r) { r = v; }

    ~F32Guard() { ref = saved; }
  };

} // namespace refractir
