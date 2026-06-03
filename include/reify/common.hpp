#pragma once

// [v0.2.2] Small shared helpers used by both rysmith and rylink.  Each
// utility here is generator policy — choices the *tools* need to make
// when driving the deterministic backends.  The C / WASM backends
// themselves are deterministic; randomness lives on this side so a
// multi-program sweep can vary backend strategies independently.
//
// Header-only by convention; everything must be `inline` (or a
// template) so this file can be included from rysmith, rylink, and
// any future generator without dragging in a `.cpp`.

#include <random>
#include <string>

namespace symir::reify {

  /**
   * Resolve a `--vec-lowering` CLI choice into a concrete strategy
   * name for the C backend.  If `requested == "random"`, picks one of
   * `{vecext, scalars, array, structscalars, structarray}` uniformly
   * via `rng`; otherwise returns `requested` verbatim so the caller
   * can hand it straight to `makeVecLowering`.  Shared between
   * rysmith and rylink so both tools sweep the same strategy set with
   * the same odds.
   */
  inline std::string pickVecLowering(std::mt19937 &rng, const std::string &requested) {
    if (requested != "random")
      return requested;
    static const char *strategies[] = {
        "vecext", "scalars", "array", "structscalars", "structarray"
    };
    std::uniform_int_distribution<int> d(0, 4);
    return strategies[d(rng)];
  }

} // namespace symir::reify
