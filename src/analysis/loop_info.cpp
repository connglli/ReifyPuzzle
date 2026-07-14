#include "analysis/loop_info.hpp"
#include <algorithm>
#include <map>
#include <ostream>

namespace refractir {

  LoopInfo LoopInfo::build(const CFG &cfg, const DomTree &dt) {
    const std::size_t n = cfg.blocks.size();
    LoopInfo li;
    li.innermostLoop.assign(n, -1);

    // 1. True back edges, grouped by header. std::map keys the headers
    //    by block index; groups are re-sorted by header RPO below so
    //    outer loops precede inner ones.
    std::map<std::size_t, std::vector<std::size_t>> latchesOf;
    for (std::size_t u = 0; u < n; ++u) {
      if (dt.rpoNumber[u] == DomTree::kNone)
        continue;
      for (std::size_t v: cfg.succ[u]) {
        if (dt.rpoNumber[v] <= dt.rpoNumber[u] && dt.dominates(v, u))
          latchesOf[v].push_back(u);
      }
    }

    std::vector<std::size_t> headers;
    for (const auto &[h, _]: latchesOf)
      headers.push_back(h);
    std::sort(headers.begin(), headers.end(), [&](std::size_t a, std::size_t b) {
      return dt.rpoNumber[a] < dt.rpoNumber[b];
    });

    const auto byRpo = [&](std::size_t a, std::size_t b) {
      return dt.rpoNumber[a] < dt.rpoNumber[b];
    };

    // 2. Natural loop bodies: backward reachability from the latches,
    //    stopping at the header.
    std::vector<std::vector<char>> inLoop;
    for (std::size_t h: headers) {
      Loop loop;
      loop.header = h;
      loop.latches = latchesOf[h];
      std::sort(loop.latches.begin(), loop.latches.end(), byRpo);

      std::vector<char> member(n, 0);
      member[h] = 1;
      std::vector<std::size_t> work;
      for (std::size_t l: loop.latches) {
        if (!member[l]) {
          member[l] = 1;
          work.push_back(l);
        }
      }
      while (!work.empty()) {
        std::size_t b = work.back();
        work.pop_back();
        for (std::size_t p: cfg.pred[b]) {
          if (!member[p] && dt.rpoNumber[p] != DomTree::kNone) {
            member[p] = 1;
            work.push_back(p);
          }
        }
      }

      for (std::size_t b = 0; b < n; ++b)
        if (member[b])
          loop.blocks.push_back(b);
      std::sort(loop.blocks.begin(), loop.blocks.end(), byRpo);

      for (std::size_t b: loop.blocks)
        for (std::size_t s: cfg.succ[b])
          if (!member[s])
            loop.exitEdges.push_back({b, s});

      inLoop.push_back(std::move(member));
      li.loops.push_back(std::move(loop));
    }

    // 3. Nesting: the parent of loop i is the containing loop with the
    //    deepest header. Containing loops have RPO-smaller headers, so
    //    they appear earlier in `loops` and their depth is final when
    //    loop i reads it.
    for (std::size_t i = 0; i < li.loops.size(); ++i) {
      int parent = -1;
      for (std::size_t j = 0; j < i; ++j) {
        if (inLoop[j][li.loops[i].header] &&
            (parent == -1 ||
             dt.rpoNumber[li.loops[j].header] > dt.rpoNumber[li.loops[parent].header]))
          parent = static_cast<int>(j);
      }
      li.loops[i].parent = parent;
      if (parent != -1) {
        li.loops[i].depth = li.loops[parent].depth + 1;
        li.loops[parent].children.push_back(static_cast<int>(i));
      }
      // Outer loops assign first; inner loops overwrite, leaving the
      // innermost index behind.
      for (std::size_t b: li.loops[i].blocks)
        li.innermostLoop[b] = static_cast<int>(i);
    }

    return li;
  }

  void LoopInfo::dump(std::ostream &os, const CFG &cfg, const std::string &funName) const {
    os << "loops " << funName << ":\n";
    if (loops.empty()) {
      os << "  (none)\n";
      return;
    }
    for (std::size_t i = 0; i < loops.size(); ++i) {
      const Loop &l = loops[i];
      os << "  loop " << i << ": header=" << cfg.blocks[l.header] << " depth=" << l.depth
         << " parent=";
      if (l.parent == -1)
        os << "none";
      else
        os << l.parent;
      os << "\n";
      os << "    latches:";
      for (std::size_t b: l.latches)
        os << " " << cfg.blocks[b];
      os << "\n";
      os << "    blocks:";
      for (std::size_t b: l.blocks)
        os << " " << cfg.blocks[b];
      os << "\n";
      os << "    exits:";
      for (const auto &[src, dst]: l.exitEdges)
        os << " " << cfg.blocks[src] << "->" << cfg.blocks[dst];
      os << "\n";
    }
  }

} // namespace refractir
