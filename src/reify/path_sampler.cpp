#include "reify/path_sampler.hpp"

#include <algorithm>
#include <deque>
#include <random>
#include <unordered_map>
#include <unordered_set>

namespace symir::reify {

  static std::optional<std::vector<std::string>>
  shortestToExit(const RyCFG &cfg, const std::string &start) {
    std::deque<std::vector<std::string>> q;
    q.push_back({start});
    std::unordered_set<std::string> visited{start};

    while (!q.empty()) {
      auto p = std::move(q.front());
      q.pop_front();
      if (p.back() == cfg.exitLabel)
        return std::vector<std::string>(p.begin() + 1, p.end());
      const auto *blk = cfg.get(p.back());
      if (!blk)
        continue;
      for (const auto &s: blk->succs) {
        if (!visited.count(s)) {
          visited.insert(s);
          auto np = p;
          np.push_back(s);
          q.push_back(std::move(np));
        }
      }
    }
    return std::nullopt;
  }

  std::optional<std::vector<std::string>>
  samplePath(const RyCFG &cfg, const SamplePathParams &params) {
    std::mt19937 rng(params.seed);

    std::vector<std::string> path{cfg.entry};
    // back_edge_count[(src, dst)] = traversal count
    std::unordered_map<std::string, std::unordered_map<std::string, int>> backEdgeCount;

    // A back edge is one whose destination already appears in the path walked so far
    auto isBackEdge = [&](const std::string &dst) -> bool {
      return std::find(path.begin(), path.end(), dst) != path.end();
    };

    for (int i = 0; i < params.maxPathLen; i++) {
      const std::string &cur = path.back();
      if (cur == cfg.exitLabel)
        return path;

      const auto *blk = cfg.get(cur);
      if (!blk || blk->succs.empty())
        break;

      // Filter: exclude back edges that hit the iteration limit
      std::vector<std::string> allowed;
      for (const auto &s: blk->succs) {
        if (isBackEdge(s)) {
          if (backEdgeCount[cur][s] < params.maxLoopIter)
            allowed.push_back(s);
        } else {
          allowed.push_back(s);
        }
      }

      if (allowed.empty()) {
        auto tail = shortestToExit(cfg, cur);
        if (!tail)
          return std::nullopt;
        path.insert(path.end(), tail->begin(), tail->end());
        return path;
      }

      std::uniform_int_distribution<int> pick(0, (int) allowed.size() - 1);
      const std::string &chosen = allowed[pick(rng)];
      if (isBackEdge(chosen))
        backEdgeCount[cur][chosen]++;
      path.push_back(chosen);
    }

    if (path.back() != cfg.exitLabel) {
      auto tail = shortestToExit(cfg, path.back());
      if (!tail)
        return std::nullopt;
      path.insert(path.end(), tail->begin(), tail->end());
    }
    return path;
  }

} // namespace symir::reify
