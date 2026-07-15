// VecLowering decorators: `structscalars` and `structarray`.
//
// Wrap the `scalars` or `array` strategy in a C struct so it can cross
// function boundaries (structs pass by value in the C ABI). The base
// strategy still drives lane access — the decorator translates the
// `name` of every base operation from `<id>` to `<id>.lanes_<id>`,
// where `lanes_<id>` is the per-instance field-block name.
//
// Layout:
//   structscalars: `struct _vec_<N>_<Telem>_scalars { T l0; T l1; …; };`
//   structarray:   `struct _vec_<N>_<Telem>_array   { T lanes[N];      };`
//
// Both register a typedef in the preamble so the C backend can use the
// short name as a single C type-string at function boundaries.

#include <set>
#include <stdexcept>
#include "backend/c_vec_lowering.hpp"

namespace refractir {

  // We re-declare these tiny helpers locally rather than pulling them in
  // from another file — each strategy file is self-contained.
  namespace {

    std::string elemSuffixStruct(const TypePtr &elem) {
      if (auto it = std::get_if<IntType>(&elem->v)) {
        int bits = it->bits.value_or(it->kind == IntType::Kind::I32 ? 32 : 64);
        return "i" + std::to_string(bits);
      }
      if (auto ft = std::get_if<FloatType>(&elem->v)) {
        return ft->kind == FloatType::Kind::F32 ? "f32" : "f64";
      }
      return "u";
    }

    std::string elemCTypeStruct(const TypePtr &elem) {
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

    std::string structTypeName(const VecType &vt, bool isArrayShape) {
      return "_vec_" + std::to_string(vt.size) + "_" + elemSuffixStruct(vt.elem) +
             (isArrayShape ? "_arr" : "_scal");
    }

    bool isConstantIdxStruct(const std::string &expr, std::uint64_t &out) {
      if (expr.empty())
        return false;
      for (char c: expr)
        if (c < '0' || c > '9')
          return false;
      try {
        out = std::stoull(expr);
        return true;
      } catch (...) {
        return false;
      }
    }

  } // namespace

  /// structarray: `struct { T lanes[N]; }`
  class StructArrayLowering : public VecLowering {
  public:
    std::string name() const override { return "structarray"; }

    void emitPreamble(std::ostream &out, const std::vector<VecType> &usedShapes) override {
      std::set<std::string> emitted;
      for (const auto &vt: usedShapes) {
        std::string tn = structTypeName(vt, /*isArrayShape=*/true);
        if (!emitted.insert(tn).second)
          continue;
        out << "typedef struct " << tn << " { " << elemCTypeStruct(vt.elem) << " lanes[" << vt.size
            << "]; } " << tn << ";\n";
      }
      if (!emitted.empty())
        out << "\n";
    }

    std::string typeString(const VecType &vt) override {
      return structTypeName(vt, /*isArrayShape=*/true);
    }

    void emitLocalDecl(std::ostream &out, const std::string &name, const VecType &vt) override {
      out << typeString(vt) << " " << name;
    }

    void emitInit(
        std::ostream &out, const std::string &name, const VecType &vt, const InitVal &iv
    ) override {
      (void) out;
      (void) name;
      (void) vt;
      (void) iv;
    }

    std::string
    emitLaneRead(const std::string &name, const VecType &vt, const std::string &idxExpr) override {
      (void) vt;
      return name + ".lanes[" + idxExpr + "]";
    }

    void emitLaneWrite(
        std::ostream &out, const std::string &name, const VecType &vt, const std::string &idxExpr,
        const std::string &valExpr
    ) override {
      out << emitLaneRead(name, vt, idxExpr) << " = " << valExpr;
    }

    void emitWholeCopy(
        std::ostream &out, const std::string &lhs, const std::string &rhs, const VecType &vt
    ) override {
      (void) vt;
      // Structs can be assigned in C; this lifts the array's restriction.
      out << lhs << " = " << rhs;
    }

    bool canCrossFnBoundary() const override { return true; }

    bool needsLaneUnroll() const override { return true; }
  };

  /// structscalars: `struct { T l0; T l1; ...; T l_{N-1}; }`
  class StructScalarsLowering : public VecLowering {
  public:
    std::string name() const override { return "structscalars"; }

    void emitPreamble(std::ostream &out, const std::vector<VecType> &usedShapes) override {
      std::set<std::string> emitted;
      for (const auto &vt: usedShapes) {
        std::string tn = structTypeName(vt, /*isArrayShape=*/false);
        if (!emitted.insert(tn).second)
          continue;
        out << "typedef struct " << tn << " {";
        for (size_t k = 0; k < vt.size; ++k) {
          out << " " << elemCTypeStruct(vt.elem) << " l" << k << ";";
        }
        out << " } " << tn << ";\n";
      }
      if (!emitted.empty())
        out << "\n";
    }

    std::string typeString(const VecType &vt) override {
      return structTypeName(vt, /*isArrayShape=*/false);
    }

    void emitLocalDecl(std::ostream &out, const std::string &name, const VecType &vt) override {
      out << typeString(vt) << " " << name;
    }

    void emitInit(
        std::ostream &out, const std::string &name, const VecType &vt, const InitVal &iv
    ) override {
      (void) out;
      (void) name;
      (void) vt;
      (void) iv;
    }

    std::string
    emitLaneRead(const std::string &name, const VecType &vt, const std::string &idxExpr) override {
      std::uint64_t k;
      if (!isConstantIdxStruct(idxExpr, k) || k >= vt.size) {
        throw std::runtime_error(
            "vec-lowering 'structscalars' does not support dynamic lane index "
            "(or static index out of range): \"" +
            idxExpr + "\""
        );
      }
      return name + ".l" + std::to_string(k);
    }

    void emitLaneWrite(
        std::ostream &out, const std::string &name, const VecType &vt, const std::string &idxExpr,
        const std::string &valExpr
    ) override {
      out << emitLaneRead(name, vt, idxExpr) << " = " << valExpr;
    }

    void emitWholeCopy(
        std::ostream &out, const std::string &lhs, const std::string &rhs, const VecType &vt
    ) override {
      (void) vt;
      // Struct assignment in C copies all fields. The compiler's struct
      // semantics give us "for free" what scalars couldn't express.
      out << lhs << " = " << rhs;
    }

    bool canCrossFnBoundary() const override { return true; }

    bool needsLaneUnroll() const override { return true; }
  };

  std::unique_ptr<VecLowering> makeStructArrayLowering() {
    return std::make_unique<StructArrayLowering>();
  }

  std::unique_ptr<VecLowering> makeStructScalarsLowering() {
    return std::make_unique<StructScalarsLowering>();
  }

} // namespace refractir
