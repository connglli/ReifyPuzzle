#pragma once

#include <optional>
#include <string>

namespace symir {

  /**
   * Represents the kind of standard built-in intrinsic.
   * Standard built-in intrinsics (§12) require no user-supplied body or contracts.
   * Semantics are hardcoded inside the interpreter, solver, and codegen backends.
   */
  enum class IntrinsicKind { Abs, Min, Max, Popcount, Clz, Ctz };

  /**
   * Resolves a global identifier string (such as "@abs") to its corresponding IntrinsicKind.
   * Returns std::nullopt if the name does not match any recognized built-in intrinsic.
   */
  inline std::optional<IntrinsicKind> getIntrinsicKind(const std::string &name) {
    if (name == "@abs")
      return IntrinsicKind::Abs;
    if (name == "@min")
      return IntrinsicKind::Min;
    if (name == "@max")
      return IntrinsicKind::Max;
    if (name == "@popcount")
      return IntrinsicKind::Popcount;
    if (name == "@clz")
      return IntrinsicKind::Clz;
    if (name == "@ctz")
      return IntrinsicKind::Ctz;
    return std::nullopt;
  }

} // namespace symir
