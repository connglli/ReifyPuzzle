#pragma once

// [v0.2.2] Function descriptor — sidecar metadata that rysmith writes
// next to each generated `.sir` file (`func_<id>_<i>.json`) so the
// future `rylink` driver can wire callers/callees without re-parsing
// the SIR source. Both the writer (used by rysmith) and the reader
// (used by rylink) live here so the on-disk schema has a single
// canonical owner.

#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>
#include "ast/ast.hpp"

namespace symir::reify {

  struct FuncDescriptor {
    // 6-char hex generation ID this descriptor was emitted under.
    std::string id;
    // Canonical sigil-prefixed function name, e.g. `@func_a3f9c2_0`.
    std::string name;
    // Return type, serialized in SIR surface syntax (e.g. `i32`,
    // `ptr i32`, `@struct_a3f9c2_0`, `<4> i32`).
    std::string retType;

    struct Param {
      std::string name; // e.g. `%pa0`
      std::string type; // SIR surface syntax
    };

    std::vector<Param> params;

    struct Sym {
      std::string name; // `%?s0`
      std::string kind; // "value" | "coef" | "index"
      std::string type;
    };

    std::vector<Sym> syms;

    // Block-label path the solver concretized along (`["^entry", "^b1",
    // "^exit"]`).
    std::vector<std::string> path;

    struct Struct {
      std::string name; // `@struct_<id>_<j>`

      struct Field {
        std::string name; // `f0`
        std::string type; // SIR surface syntax
      };

      std::vector<Field> fields;
    };

    std::vector<Struct> structs;

    // Filenames of the concrete `.sir` realizations the solver
    // produced for this function (relative to the descriptor's
    // directory). May be more than one when rysmith used `--n-inits`.
    std::vector<std::string> concretes;
  };

  // Serialize a descriptor as JSON to `outPath`. Overwrites any
  // existing file. Throws on I/O error.
  void writeFuncDescriptor(const std::filesystem::path &outPath, const FuncDescriptor &d);

  // Convenience overload that builds the FuncDescriptor from an
  // in-memory Program + bookkeeping the caller already has. The
  // function named `@<funcName>` must exist in `prog.funs`; if it
  // doesn't, nothing is written and `false` is returned.
  bool writeFuncDescriptorFromProgram(
      const std::filesystem::path &outPath, const std::string &funcName, const symir::Program &prog,
      const std::vector<std::string> &pathLabels,
      const std::vector<std::filesystem::path> &concreteFiles, const std::string &genId
  );

  // Parse a descriptor JSON. Returns nullopt on parse error (callers
  // typically log and skip). The JSON shape is intentionally narrow
  // (flat string-valued fields plus a few small arrays) so a tiny
  // hand-written parser is sufficient — no dependency added.
  std::optional<FuncDescriptor> readFuncDescriptor(const std::filesystem::path &path);
  std::optional<FuncDescriptor> parseFuncDescriptor(const std::string &json);

} // namespace symir::reify
