#pragma once

// Program-level rewrite framework (top tier).
//
// A `Pass` is a composable whole-program transformation over a RefractIR
// `Program`, threading reify metadata (per-function descriptors + state
// profiles) and, for synthesizing passes, an SMT solver factory. Concrete
// passes either preserve the descriptor's input->output contract
// (equivalence on the profiled input) or document precisely what they
// change.
//
// This is the tier the user calls "a kind of rewrite": rytwin's
// equivalence-preserving block graft, the crc32-checksum rewrite, the
// @main-wrapper injection, and rylink's call-realization are all `Pass`es.
// The finer peephole engine in reify/rewrite.hpp (RewriteRule / RewriteEngine)
// stays as-is and becomes an implementation detail of the one pass that
// needs it (call-realization); it is deliberately NOT unified into this
// header.

#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "ast/ast.hpp"
#include "reify/func_desc.hpp"
#include "reify/state_profile.hpp"
#include "solver/solver.hpp"

namespace refractir::reify {

  // Shared, threaded context for a chain of Passes. A single-function tool
  // (rytwin) fills one entry in each map keyed by the entry function's
  // canonical name; rylink whole-programs carry many.
  struct PassCtx {
    std::mt19937 rng;                              // deterministic (seeded by the tool)
    SymbolicExecutor::SolverFactory solverFactory; // for synthesizing passes (rytwin); may be null
    std::unordered_map<std::string, FuncDescriptor> descriptors; // input i / oracle, by func name
    std::unordered_map<std::string, StateProfile> profiles;      // per-point state, by func name
  };

  // Outcome of applying one Pass (or a whole pipeline).
  struct PassReport {
    std::size_t sites = 0; // number of rewrite sites applied
    bool ok = true;        // false aborts the pipeline
    std::string message;   // human-readable note on failure / summary
  };

  class Pass {
  public:
    virtual ~Pass() = default;
    virtual std::string_view name() const = 0;

    // A pass that needs the per-function StateProfile (e.g. rytwin) says so
    // here; the driver can then refuse to run it without a profile rather
    // than silently no-op.
    virtual bool needsProfile() const { return false; }

    virtual PassReport apply(Program &prog, PassCtx &ctx) = 0;
  };

  // Ordered application, threading `ctx` through each stage. Stops at the
  // first pass that returns ok == false; otherwise sums the site counts.
  class PassPipeline {
  public:
    PassPipeline &add(std::unique_ptr<Pass> pass) {
      passes_.push_back(std::move(pass));
      return *this;
    }

    PassReport run(Program &prog, PassCtx &ctx);

  private:
    std::vector<std::unique_ptr<Pass>> passes_;
  };

} // namespace refractir::reify
