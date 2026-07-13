#pragma once

// StateProfile — a per-program-point record of the concrete program state
// a RefractIR program passes through under a specific input.
//
// rysmith generates a program `p1` together with the exact input `i` that
// concretizes it, so the whole execution is deterministic and known. A
// StateProfile captures the value of every initialized local / parameter at
// each executed program point (block entry, or after each instruction) by
// running the reference interpreter with a state-capture hook. It is the
// data an equivalence-preserving rewrite needs: to synthesize a block `B'`
// that reproduces `B`'s effect on the one state `B` actually sees, we must
// first know that state (`s` at B's entry) and the state it produces (`s'`
// at B's exit == the next block's entry).
//
// Serialized as a JSON sidecar by `rysmith --emit-state` and consumed by
// `rytwin`. Float leaves are carried as canonical decimal strings via
// refractir::formatDouble so the profile is bit-exact across the text
// boundary (see the FP-serialization invariant in CLAUDE.md).

#include <cstdint>
#include <iosfwd>
#include <string>
#include <utility>
#include <vector>

#include "ast/ast.hpp"
#include "interp/value.hpp"

namespace refractir {
  class Program;
}

namespace refractir::reify {

  // Granularity of the captured trace. The CLI / JSON tokens are "pbb"
  // (per basic block) and "ppp" (per program point).
  enum class StateGranularity {
    Pbb, // one record per executed block, at its entry
    Ppp, // additionally one record after each instruction
  };

  // A concrete value tree mirroring the RefractIR type shape. Pointer and
  // uninitialized (undef) leaves are represented as their own kinds and
  // carry no value — rewrites that need pure scalar/vector state ignore
  // them. Scalars keep their bit-width; float scalars keep a canonical
  // decimal string (bit-exact) alongside the raw double.
  struct StateValue {
    enum class Kind { Int, Float, Array, Vec, Struct, Ptr, Undef } kind = Kind::Undef;
    std::int64_t intVal = 0;
    double floatVal = 0.0;
    std::uint32_t bits = 0;                                 // scalar bit-width (Int / Float)
    std::vector<StateValue> elems;                          // Array / Vec, in order
    std::vector<std::pair<std::string, StateValue>> fields; // Struct, in declaration order
  };

  // One captured program point.
  struct StatePoint {
    std::string block; // block label, e.g. "^b3"
    int instr = -1;    // -1 = block entry; >= 0 = after instruction `instr`
    // Initialized locals + parameters visible at this point, in a stable
    // (sorted-by-name) order so two runs serialize identically.
    std::vector<std::pair<std::string, StateValue>> vars;
  };

  struct StateProfile {
    std::string func; // entry function name (canonical, "@...")
    StateGranularity granularity = StateGranularity::Pbb;
    std::vector<StatePoint> trace; // in execution order
  };

  // Convert one interpreter RuntimeValue into a StateValue tree.
  StateValue toStateValue(const RuntimeValue &rv);

  // Run `prog`'s entry `func` under `paramArgs` (one decimal-int / hex-float
  // string per parameter, declaration order) with the given granularity and
  // return the resulting StateProfile. Throws whatever the interpreter
  // throws (e.g. UndefinedBehaviorError) — callers targeting UB-free
  // programs can let it propagate.
  StateProfile profileProgram(
      const Program &prog, const std::string &func, const std::vector<std::string> &paramArgs,
      StateGranularity granularity = StateGranularity::Pbb
  );

  // Serialize a StateProfile to JSON (bit-exact FP via formatDouble).
  void writeStateProfileJson(std::ostream &os, const StateProfile &profile);

} // namespace refractir::reify
