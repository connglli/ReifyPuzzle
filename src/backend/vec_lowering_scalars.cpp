// VecLowering strategy: N separate scalar locals.
//
// `<N> T name` lowers to `T name_0; T name_1; ...; T name_{N-1};`. Lane
// access at constant index k is `name_k`. Dynamic indices are rejected
// (the C language has no way to index a set of separate locals at
// runtime without an explicit table or switch — out of scope here).
//
// This is the most opaque-to-C-compiler strategy: each lane is a
// separate named identifier, so the C compiler sees no relationship
// between them. Cannot cross function boundaries (no single C type
// for N separate scalars). The `structscalars` decorator wraps the
// N fields in a struct to lift that restriction.

#include <stdexcept>
#include "backend/vec_lowering.hpp"

namespace symir {

  namespace {

    std::string elemCTypeForScal(const TypePtr &elem) {
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

    // Parse a C expression as a non-negative integer if it is one. The
    // `scalars` strategy needs to refuse dynamic indices.
    bool isConstantIdx(const std::string &expr, std::uint64_t &out) {
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

  class ScalarsLowering : public VecLowering {
  public:
    std::string name() const override { return "scalars"; }

    void emitPreamble(std::ostream &out, const std::vector<VecType> &usedShapes) override {
      (void) out;
      (void) usedShapes; // no preamble; declarations are per-local.
    }

    std::string typeString(const VecType &vt) override {
      (void) vt;
      return ""; // no single C type for N separate scalars.
    }

    void emitLocalDecl(std::ostream &out, const std::string &name, const VecType &vt) override {
      // Emit N declarations on one line, semicolon-separated. The C
      // backend's caller wraps the whole thing in a single statement
      // terminator (one trailing `;`), so we use commas within a single
      // declaration list to keep the existing emission contract.
      out << elemCTypeForScal(vt.elem) << " " << name << "_0";
      for (size_t k = 1; k < vt.size; ++k)
        out << ", " << name << "_" << k;
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
      std::uint64_t k;
      if (!isConstantIdx(idxExpr, k) || k >= vt.size) {
        throw std::runtime_error(
            "vec-lowering 'scalars' does not support dynamic lane index "
            "(or static index out of range): \"" +
            idxExpr + "\""
        );
      }
      return name + "_" + std::to_string(k);
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
      // N separate copies on a single line (the caller closes with `;\n`).
      for (size_t k = 0; k < vt.size; ++k) {
        if (k)
          out << "; ";
        out << lhs << "_" << k << " = " << rhs << "_" << k;
      }
    }

    bool canCrossFnBoundary() const override { return false; }

    bool needsLaneUnroll() const override { return true; }
  };

  std::unique_ptr<VecLowering> makeScalarsLowering() { return std::make_unique<ScalarsLowering>(); }

} // namespace symir
