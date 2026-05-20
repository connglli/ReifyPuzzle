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

    // Identify static back edges (dst appears earlier in the label order than src).
    // The CFG is built so that label-order matches a forward spanning chain
    // (entry -> b0 -> ... -> exit), so these edges form the loops.
    std::vector<std::pair<std::string, std::string>> staticBackEdges;
    for (std::size_t i = 0; i < cfg.blocks.size(); i++) {
      const auto &blk = cfg.blocks[i];
      for (const auto &s: blk.succs) {
        auto it = cfg.blockIndex.find(s);
        if (it != cfg.blockIndex.end() && it->second <= i)
          staticBackEdges.push_back({blk.label, s});
      }
    }

    // If a minimum is requested but the CFG has no loops, we cannot satisfy it.
    if (params.minLoopIter > 0 && staticBackEdges.empty())
      return std::nullopt;

    // Pick a single target back edge to force-traverse the required number of times.
    std::string targetSrc, targetDst;
    std::unordered_set<std::string> canReachTarget;
    if (params.minLoopIter > 0) {
      std::uniform_int_distribution<std::size_t> pick(0, staticBackEdges.size() - 1);
      auto [s, d] = staticBackEdges[pick(rng)];
      targetSrc = s;
      targetDst = d;

      // BFS in reverse from targetSrc to mark blocks that can reach it.
      // The walk will be biased toward these while iterations are still owed.
      std::unordered_map<std::string, std::vector<std::string>> rev;
      for (const auto &b: cfg.blocks)
        for (const auto &succ: b.succs)
          rev[succ].push_back(b.label);
      std::deque<std::string> q{targetSrc};
      canReachTarget.insert(targetSrc);
      while (!q.empty()) {
        auto cur = q.front();
        q.pop_front();
        for (const auto &p: rev[cur])
          if (canReachTarget.insert(p).second)
            q.push_back(p);
      }
    }

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
        break;

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
        break;
      }

      // While we still owe iterations on the target back edge: at targetSrc
      // force-take the back edge; elsewhere bias the random walk toward
      // successors that can still reach targetSrc.
      std::string chosen;
      bool forced = false;
      if (!targetSrc.empty() && backEdgeCount[targetSrc][targetDst] < params.minLoopIter) {
        if (cur == targetSrc &&
            std::find(allowed.begin(), allowed.end(), targetDst) != allowed.end()) {
          chosen = targetDst;
          forced = true;
        } else {
          std::vector<std::string> biased;
          for (const auto &s: allowed)
            if (canReachTarget.count(s))
              biased.push_back(s);
          if (!biased.empty()) {
            std::uniform_int_distribution<int> pick(0, (int) biased.size() - 1);
            chosen = biased[pick(rng)];
            forced = true;
          }
        }
      }
      if (!forced) {
        std::uniform_int_distribution<int> pick(0, (int) allowed.size() - 1);
        chosen = allowed[pick(rng)];
      }

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

    // Safety net: if the forced edge never got enough iterations (e.g. ran out
    // of maxPathLen budget), reject so the caller can retry with a fresh seed.
    if (params.minLoopIter > 0 && backEdgeCount[targetSrc][targetDst] < params.minLoopIter)
      return std::nullopt;

    return path;
  }

} // namespace symir::reify
