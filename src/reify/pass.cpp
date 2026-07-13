#include "reify/pass.hpp"

namespace refractir::reify {

  PassReport PassPipeline::run(Program &prog, PassCtx &ctx) {
    PassReport total;
    for (auto &pass: passes_) {
      PassReport r = pass->apply(prog, ctx);
      total.sites += r.sites;
      if (!r.ok) {
        total.ok = false;
        total.message = std::string(pass->name()) + ": " + r.message;
        return total;
      }
    }
    return total;
  }

} // namespace refractir::reify
