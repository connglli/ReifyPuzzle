// [v0.2.3] "array" WASM vec-lowering: a vector `<N> T` local is a
// shadow-stack aggregate (the historical storage). The strategy object
// only *classifies* — every access keeps the backend's frame-memory
// address+load/store paths, so the lane hooks must never be reached.

#include <stdexcept>
#include "backend/wasm_backend.hpp"
#include "backend/wasm_vec_lowering.hpp"

namespace refractir {

  std::ostream &WasmVecLowering::out(WasmBackend &b) { return b.out_; }

  void WasmVecLowering::indent(WasmBackend &b) { b.indent(); }

  namespace {

    class WasmArrayLowering final : public WasmVecLowering {
    public:
      std::string name() const override { return "array"; }

      bool usesFrameMemory() const override { return true; }

      [[noreturn]] static void unreached(const char *op) {
        throw std::logic_error(
            std::string("wasm vec-lowering 'array': ") + op +
            " hook reached — frame-memory accesses must use the backend's aggregate paths"
        );
      }

      void declareLocals(WasmBackend &, const std::string &, const VecType &) override {
        unreached("declareLocals");
      }

      void unpackParam(WasmBackend &, const std::string &, const VecType &) override {
        unreached("unpackParam");
      }

      void
      emitLaneRead(WasmBackend &, const std::string &, const VecType &, std::uint64_t) override {
        unreached("emitLaneRead");
      }

      void emitLaneReadDyn(
          WasmBackend &, const std::string &, const VecType &, const std::function<void()> &
      ) override {
        unreached("emitLaneReadDyn");
      }

      void emitLaneWrite(
          WasmBackend &, const std::string &, const VecType &, std::uint64_t,
          const std::function<void()> &
      ) override {
        unreached("emitLaneWrite");
      }

      void emitLaneWriteDyn(
          WasmBackend &, const std::string &, const VecType &, const std::function<void()> &,
          const std::function<void()> &
      ) override {
        unreached("emitLaneWriteDyn");
      }
    };

  } // namespace

  std::unique_ptr<WasmVecLowering> makeWasmVecLowering(const std::string &name) {
    if (name == "array")
      return std::make_unique<WasmArrayLowering>();
    return nullptr;
  }

} // namespace refractir
