#pragma once

#include <iosfwd>
#include <string>
#include <vector>
#include "ast/ast.hpp"

namespace refractir {

  class PyBackend;

  /**
   * Python-side intrinsic registry: emits one generic,
   * width-parameterized helper per IntrinsicKind used by the program,
   * and builds the call expression for a resolved intrinsic call site
   * (widths taken from the IntrinsicDecl the type checker pinned).
   * Semantics mirror src/interp/intrinsics.cpp.
   */
  struct PyIntrinsicRegistry {
    static void emitHelpers(std::ostream &out, const Program &prog);
    static std::string
    call(const PyBackend &backend, const IntrinsicDecl &intr, const std::vector<std::string> &args);
  };

} // namespace refractir
