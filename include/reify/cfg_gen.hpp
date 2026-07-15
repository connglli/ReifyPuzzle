#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace refractir::reify {

  struct RyCFGBlock {
    std::string label;
    std::vector<std::string> succs;
    std::vector<std::string> preds;

    bool isBranch() const { return succs.size() == 2; }

    bool isGoto() const { return succs.size() == 1; }

    bool isTerminal() const { return succs.empty(); }
  };

  struct RyCFG {
    std::string entry;
    std::string exitLabel;
    std::vector<RyCFGBlock> blocks;
    std::unordered_map<std::string, std::size_t> blockIndex;

    RyCFGBlock *get(const std::string &label);
    const RyCFGBlock *get(const std::string &label) const;
    void addEdge(const std::string &src, const std::string &dst);
    void removeEdge(const std::string &src, const std::string &dst);
    std::vector<std::string> labels() const;
  };

  struct GenCFGParams {
    int nBbls = 15;
    uint32_t seed = 0;
    double pBranch = 0.5;
    double pBackedge = 0.3;
    int maxBackEdges = 2;
    // [v0.2.3] Repair the generated CFG to be reducible: retreating
    // edges whose target does not dominate their source are deleted
    // (the forward skeleton and all valid loops survive). Required by
    // structuring consumers (python target, C --structured-lowering).
    bool requireReducible = false;
  };

  RyCFG genCFG(const GenCFGParams &params);

} // namespace refractir::reify
