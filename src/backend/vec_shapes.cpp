#include "backend/vec_shapes.hpp"

#include <variant>

namespace refractir {

  namespace {

    void collectVecShapesInType(const TypePtr &t, std::vector<VecType> &out) {
      if (!t)
        return;
      std::visit(
          [&](auto &&arg) {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, VecType>) {
              out.push_back(arg);
              collectVecShapesInType(arg.elem, out);
            } else if constexpr (std::is_same_v<T, ArrayType>) {
              collectVecShapesInType(arg.elem, out);
            } else if constexpr (std::is_same_v<T, PtrType>) {
              collectVecShapesInType(arg.pointee, out);
            }
          },
          t->v
      );
    }

  } // namespace

  std::vector<VecType> collectVecShapes(const Program &prog) {
    std::vector<VecType> out;
    for (const auto &s: prog.structs)
      for (const auto &f: s.fields)
        collectVecShapesInType(f.type, out);
    for (const auto &f: prog.funs) {
      collectVecShapesInType(f.retType, out);
      for (const auto &p: f.params)
        collectVecShapesInType(p.type, out);
      for (const auto &s: f.syms)
        collectVecShapesInType(s.type, out);
      for (const auto &l: f.lets)
        collectVecShapesInType(l.type, out);
    }
    return out;
  }

} // namespace refractir
