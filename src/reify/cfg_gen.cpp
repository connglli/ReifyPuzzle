#include "reify/cfg_gen.hpp"

#include <algorithm>
#include <random>

#include "analysis/dominators.hpp"
#include "analysis/reducibility.hpp"

namespace refractir::reify {

  namespace {

    // Adapt an RyCFG to the analysis CFG shape (a plain label/index
    // graph) so DomTree and ReducibilityResult are reused verbatim —
    // neither touches the AST behind CFG::build's factory.
    CFG toAnalysisCFG(const RyCFG &ry) {
      CFG cfg;
      cfg.blocks = ry.labels();
      cfg.succ.resize(cfg.blocks.size());
      cfg.pred.resize(cfg.blocks.size());
      for (std::size_t i = 0; i < cfg.blocks.size(); ++i)
        cfg.indexOf[cfg.blocks[i]] = i;
      for (std::size_t i = 0; i < ry.blocks.size(); ++i)
        for (const auto &s: ry.blocks[i].succs) {
          std::size_t j = cfg.indexOf.at(s);
          cfg.succ[i].push_back(j);
          cfg.pred[j].push_back(i);
        }
      cfg.entry = cfg.indexOf.at(ry.entry);
      return cfg;
    }

    // Delete offending retreating edges until every remaining one is a
    // true back edge. One offender per pass: removing an edge only
    // shrinks path sets, so dominance can only grow and a co-offender
    // may become a valid back edge — re-analyzing preserves the
    // maximal loop set. The spanning chain is never an offender, so
    // each pass removes one optional edge, connectivity is preserved,
    // and the loop terminates.
    void repairToReducible(RyCFG &ry) {
      for (;;) {
        CFG cfg = toAnalysisCFG(ry);
        DomTree dt = DomTree::build(cfg);
        ReducibilityResult res = ReducibilityResult::check(cfg, dt);
        if (res.reducible())
          return;
        const auto &off = res.offenders.front();
        ry.removeEdge(cfg.blocks[off.src], cfg.blocks[off.dst]);
      }
    }

  } // namespace

  RyCFGBlock *RyCFG::get(const std::string &label) {
    auto it = blockIndex.find(label);
    return it == blockIndex.end() ? nullptr : &blocks[it->second];
  }

  const RyCFGBlock *RyCFG::get(const std::string &label) const {
    auto it = blockIndex.find(label);
    return it == blockIndex.end() ? nullptr : &blocks[it->second];
  }

  void RyCFG::addEdge(const std::string &src, const std::string &dst) {
    auto *s = get(src);
    auto *d = get(dst);
    if (!s || !d)
      return;
    if (std::find(s->succs.begin(), s->succs.end(), dst) == s->succs.end())
      s->succs.push_back(dst);
    if (std::find(d->preds.begin(), d->preds.end(), src) == d->preds.end())
      d->preds.push_back(src);
  }

  void RyCFG::removeEdge(const std::string &src, const std::string &dst) {
    auto *s = get(src);
    auto *d = get(dst);
    if (!s || !d)
      return;
    s->succs.erase(std::remove(s->succs.begin(), s->succs.end(), dst), s->succs.end());
    d->preds.erase(std::remove(d->preds.begin(), d->preds.end(), src), d->preds.end());
  }

  std::vector<std::string> RyCFG::labels() const {
    std::vector<std::string> labs;
    labs.reserve(blocks.size());
    for (const auto &b: blocks)
      labs.push_back(b.label);
    return labs;
  }

  RyCFG genCFG(const GenCFGParams &params) {
    std::mt19937 rng(params.seed);
    std::uniform_real_distribution<double> prob(0.0, 1.0);

    int nBbls = std::max(0, params.nBbls);

    std::vector<std::string> labels;
    labels.push_back("entry");
    for (int i = 0; i < nBbls; i++)
      labels.push_back("b" + std::to_string(i));
    labels.push_back("exit");

    RyCFG cfg;
    cfg.entry = "entry";
    cfg.exitLabel = "exit";
    for (const auto &lbl: labels) {
      cfg.blockIndex[lbl] = cfg.blocks.size();
      cfg.blocks.push_back(RyCFGBlock{lbl, {}, {}});
    }

    // Spanning chain: entry -> b0 -> ... -> b_{n-1} -> exit
    for (std::size_t i = 0; i + 1 < labels.size(); i++)
      cfg.addEdge(labels[i], labels[i + 1]);

    // Optional second forward successor (branch)
    for (std::size_t i = 0; i + 1 < labels.size(); i++) {
      auto *blk = cfg.get(labels[i]);
      if (blk->succs.size() >= 2)
        continue;
      if (prob(rng) < params.pBranch) {
        // Direct successor is labels[i+1]; pick a further forward target
        std::size_t directNextIdx = i + 1;
        std::vector<std::string> cands(labels.begin() + directNextIdx + 1, labels.end());
        if (!cands.empty()) {
          std::uniform_int_distribution<int> pick(0, (int) cands.size() - 1);
          cfg.addEdge(labels[i], cands[pick(rng)]);
        }
      }
    }

    // Optional back edges (loops) — only from interior blocks, max maxBackEdges
    int backCount = 0;
    std::vector<std::string> interior(labels.begin() + 1, labels.end() - 1);
    std::shuffle(interior.begin(), interior.end(), rng);

    for (const auto &lbl: interior) {
      if (backCount >= params.maxBackEdges)
        break;
      auto *blk = cfg.get(lbl);
      if (blk->succs.size() >= 2)
        continue;
      if (prob(rng) < params.pBackedge) {
        auto it = std::find(labels.begin(), labels.end(), lbl);
        std::size_t idx = (std::size_t) (it - labels.begin());
        std::vector<std::string> cands(labels.begin(), labels.begin() + idx);
        if (!cands.empty()) {
          std::uniform_int_distribution<int> pick(0, (int) cands.size() - 1);
          cfg.addEdge(lbl, cands[pick(rng)]);
          ++backCount;
        }
      }
    }

    if (params.requireReducible)
      repairToReducible(cfg);

    return cfg;
  }

} // namespace refractir::reify
