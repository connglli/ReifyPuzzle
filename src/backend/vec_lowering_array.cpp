// VecLowering strategy: plain `T[N]` C array.
//
// `<N> T` lowers to `T name[N];`. Lane access uses C subscript `name[k]`.
// Arithmetic is emitted lane-by-lane at AssignInstr level (the C backend
// dispatches when `needsLaneUnroll()` is true).
//
// Cannot cross function boundaries: C decays arrays to pointers in
// parameter and return positions, breaking by-value semantics. The
// `structarray` decorator lifts this restriction.

#include "backend/vec_lowering.hpp"

namespace refractir {

  namespace {

    std::string elemCTypeForArr(const TypePtr &elem) {
      if (auto it = std::get_if<IntType>(&elem->v)) {
        int bits = it->bits.value_or(it->kind == IntType::Kind::I32 ? 32 : 64);
        if (bits <= 8)
          return "int8_t";
        if (bits <= 16)
          return "int16_t";
        if (bits <= 32)
          return "int32_t";
        return "int64_t";
      }
      if (auto ft = std::get_if<FloatType>(&elem->v)) {
        return ft->kind == FloatType::Kind::F32 ? "float" : "double";
      }
      return "void";
    }

  } // namespace

  class ArrayLowering : public VecLowering {
  public:
    std::string name() const override { return "array"; }

    void emitPreamble(std::ostream &out, const std::vector<VecType> &usedShapes) override {
      (void) out;
      (void) usedShapes; // arrays declared inline at locals; no preamble.
    }

    std::string typeString(const VecType &vt) override {
      (void) vt; // `T[N]` has no single-token form usable at function boundaries.
      return "";
    }

    void emitLocalDecl(std::ostream &out, const std::string &name, const VecType &vt) override {
      out << elemCTypeForArr(vt.elem) << " " << name << "[" << vt.size << "]";
    }

    void emitInit(
        std::ostream &out, const std::string &name, const VecType &vt, const InitVal &iv
    ) override {
      (void) out;
      (void) name;
      (void) vt;
      (void) iv; // brace-init driven by the C backend.
    }

    std::string
    emitLaneRead(const std::string &name, const VecType &vt, const std::string &idxExpr) override {
      (void) vt;
      return name + "[" + idxExpr + "]";
    }

    void emitLaneWrite(
        std::ostream &out, const std::string &name, const VecType &vt, const std::string &idxExpr,
        const std::string &valExpr
    ) override {
      (void) vt;
      out << name << "[" << idxExpr << "] = " << valExpr;
    }

    void emitWholeCopy(
        std::ostream &out, const std::string &lhs, const std::string &rhs, const VecType &vt
    ) override {
      // C does not allow `a = b` between two arrays.
      out << "for (int _k = 0; _k < " << vt.size << "; ++_k) " << lhs << "[_k] = " << rhs << "[_k]";
    }

    bool canCrossFnBoundary() const override { return false; }

    bool needsLaneUnroll() const override { return true; }
  };

  std::unique_ptr<VecLowering> makeArrayLowering() { return std::make_unique<ArrayLowering>(); }

} // namespace refractir
