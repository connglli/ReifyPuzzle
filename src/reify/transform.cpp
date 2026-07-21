#include "reify/transform.hpp"

namespace refractir::reify {

  TransformReport TransformPipeline::run(Program &prog, TransformContext &ctx) {
    TransformReport total;
    for (auto &t: transforms_) {
      TransformReport r = t->apply(prog, ctx);
      total.sites += r.sites;
      if (!r.ok) {
        total.ok = false;
        total.message = std::string(t->name()) + ": " + r.message;
        return total;
      }
    }
    return total;
  }

} // namespace refractir::reify
