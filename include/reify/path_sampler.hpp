#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include "reify/cfg_gen.hpp"

namespace symir::reify {

  struct SamplePathParams {
    uint32_t seed = 0;
    int maxLoopIter = 1;
    int maxPathLen = 50;
  };

  std::optional<std::vector<std::string>>
  samplePath(const RyCFG &cfg, const SamplePathParams &params);

} // namespace symir::reify
