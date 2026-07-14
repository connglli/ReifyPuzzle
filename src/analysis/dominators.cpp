#include "analysis/dominators.hpp"
#include <ostream>

namespace refractir {

  namespace {

    // Walks two blocks up the idom chain until they meet. Ancestors
    // always carry smaller RPO numbers, so the deeper block steps up.
    std::size_t intersect(
        const std::vector<std::size_t> &idom, const std::vector<std::size_t> &rpoNumber,
        std::size_t a, std::size_t b
    ) {
      while (a != b) {
        while (rpoNumber[a] > rpoNumber[b])
          a = idom[a];
        while (rpoNumber[b] > rpoNumber[a])
          b = idom[b];
      }
      return a;
    }

  } // namespace

  DomTree DomTree::build(const CFG &cfg) {
    const std::size_t n = cfg.blocks.size();
    DomTree dt;
    dt.idom.assign(n, kNone);
    dt.rpoNumber.assign(n, kNone);
    dt.children.assign(n, {});
    if (n == 0)
      return dt;

    const std::vector<std::size_t> order = cfg.rpo();
    for (std::size_t i = 0; i < order.size(); ++i)
      dt.rpoNumber[order[i]] = i;

    dt.idom[cfg.entry] = cfg.entry;
    bool changed = true;
    while (changed) {
      changed = false;
      for (std::size_t b: order) {
        if (b == cfg.entry)
          continue;
        // Predecessor lists may contain unreachable blocks (a dead
        // block can still branch into live code); those never get an
        // idom and must not take part in the intersection.
        std::size_t newIdom = kNone;
        for (std::size_t p: cfg.pred[b]) {
          if (dt.idom[p] == kNone)
            continue;
          newIdom = newIdom == kNone ? p : intersect(dt.idom, dt.rpoNumber, newIdom, p);
        }
        if (newIdom != kNone && dt.idom[b] != newIdom) {
          dt.idom[b] = newIdom;
          changed = true;
        }
      }
    }

    for (std::size_t b: order) {
      if (b != cfg.entry)
        dt.children[dt.idom[b]].push_back(b); // RPO iteration keeps children RPO-sorted
    }
    return dt;
  }

  bool DomTree::dominates(std::size_t a, std::size_t b) const {
    if (a >= idom.size() || b >= idom.size() || idom[a] == kNone || idom[b] == kNone)
      return false;
    while (rpoNumber[b] > rpoNumber[a])
      b = idom[b];
    return a == b;
  }

  void DomTree::dump(std::ostream &os, const CFG &cfg, const std::string &funName) const {
    os << "domtree " << funName << ":\n";
    for (std::size_t b = 0; b < cfg.blocks.size(); ++b) {
      os << "  " << cfg.blocks[b] << ": ";
      if (idom[b] == kNone)
        os << "(unreachable)";
      else if (b == cfg.entry)
        os << "(root)";
      else
        os << "idom=" << cfg.blocks[idom[b]];
      os << "\n";
    }
  }

} // namespace refractir
