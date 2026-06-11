#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include "reify/cfg_gen.hpp"

namespace refractir::reify {

  struct SamplePathParams {
    uint32_t seed = 0;
    int maxLoopIter = 1;
    // If > 0, the sampler guarantees that at least one back edge in the
    // returned path is traversed at least this many times. Returns nullopt
    // if the CFG has no back edges (so the caller can retry with a new CFG).
    int minLoopIter = 0;
    int maxPathLen = 50;
  };

  std::optional<std::vector<std::string>>
  samplePath(const RyCFG &cfg, const SamplePathParams &params);

} // namespace refractir::reify
