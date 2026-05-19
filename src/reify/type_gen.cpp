#include "reify/type_gen.hpp"

#include <cassert>
#include <stdexcept>

namespace symir::reify {

  // ---------------------------------------------------------------------------
  // Factory helpers
  // ---------------------------------------------------------------------------

  static TypePtr makeIntTypeOfBits(uint32_t bits) {
    IntType t;
    switch (bits) {
      case 8:
        t.kind = IntType::Kind::ICustom;
        t.bits = 8;
        break;
      case 16:
        t.kind = IntType::Kind::ICustom;
        t.bits = 16;
        break;
      case 32:
        t.kind = IntType::Kind::I32;
        break;
      case 64:
        t.kind = IntType::Kind::I64;
        break;
      default:
        t.kind = IntType::Kind::ICustom;
        t.bits = (int) bits;
        break;
    }
    return std::make_shared<Type>(Type{t, {}});
  }

  // ---------------------------------------------------------------------------
  // Public helpers
  // ---------------------------------------------------------------------------

  bool isIntType(const TypePtr &t) { return t && std::holds_alternative<IntType>(t->v); }

  bool isFpType(const TypePtr &t) { return t && std::holds_alternative<FloatType>(t->v); }

  bool isPtrType(const TypePtr &t) { return t && std::holds_alternative<PtrType>(t->v); }

  bool isAggType(const TypePtr &t) {
    if (!t)
      return false;
    return std::holds_alternative<ArrayType>(t->v) || std::holds_alternative<StructType>(t->v);
  }

  bool isScalarType(const TypePtr &t) { return isIntType(t) || isFpType(t); }

  uint32_t intBitWidth(const TypePtr &t) {
    assert(isIntType(t));
    const auto &it = std::get<IntType>(t->v);
    switch (it.kind) {
      case IntType::Kind::I32:
        return 32;
      case IntType::Kind::I64:
        return 64;
      case IntType::Kind::ICustom:
        return (uint32_t) *it.bits;
    }
    return 32;
  }

  TypePtr pointeeType(const TypePtr &t) {
    assert(isPtrType(t));
    return std::get<PtrType>(t->v).pointee;
  }

  bool typeEquals(const TypePtr &a, const TypePtr &b) {
    if (!a || !b)
      return a == b;
    // Compare by index first
    if (a->v.index() != b->v.index())
      return false;

    if (isIntType(a)) {
      return intBitWidth(a) == intBitWidth(b);
    }
    if (isFpType(a)) {
      auto fa = std::get<FloatType>(a->v).kind;
      auto fb = std::get<FloatType>(b->v).kind;
      return fa == fb;
    }
    if (isPtrType(a)) {
      return typeEquals(std::get<PtrType>(a->v).pointee, std::get<PtrType>(b->v).pointee);
    }
    if (std::holds_alternative<ArrayType>(a->v)) {
      const auto &aa = std::get<ArrayType>(a->v);
      const auto &ab = std::get<ArrayType>(b->v);
      return aa.size == ab.size && typeEquals(aa.elem, ab.elem);
    }
    if (std::holds_alternative<StructType>(a->v)) {
      // struct equality by name
      return std::get<StructType>(a->v).name.name == std::get<StructType>(b->v).name.name;
    }
    return false;
  }

  // ---------------------------------------------------------------------------
  // Generators
  // ---------------------------------------------------------------------------

  TypePtr genIntType(std::mt19937 &rng) {
    static const uint32_t widths[] = {8, 16, 32, 64};
    std::uniform_int_distribution<int> d(0, 3);
    return makeIntTypeOfBits(widths[d(rng)]);
  }

  TypePtr genScalarType(std::mt19937 &rng, bool enableFp) {
    // Integer types: i8, i16, i32, i64 — equal probability (each 1 slot)
    // Float types (f32, f64 combined) get the same probability as ONE integer type
    // Total slots: 5 if fp enabled (4 int + 1 fp bucket), 4 if not
    std::uniform_int_distribution<int> d(0, enableFp ? 4 : 3);
    int pick = d(rng);
    if (pick < 4) {
      static const uint32_t widths[] = {8, 16, 32, 64};
      return makeIntTypeOfBits(widths[pick]);
    }
    // Float bucket: pick f32 or f64
    std::uniform_int_distribution<int> fpick(0, 1);
    FloatType ft;
    ft.kind = fpick(rng) ? FloatType::Kind::F64 : FloatType::Kind::F32;
    return std::make_shared<Type>(Type{ft, {}});
  }

  TypePtr genRandomType(std::mt19937 &rng, const TypeGenConfig &cfg, int depth) {
    // Probability buckets (sum to 1):
    // depth 0: ~50% scalar, ~20% array, ~15% struct, ~15% ptr
    // Reduce agg probability at maxAggNesting, ptr probability at maxPtrDepth
    double pScalar = 0.50;
    double pArray = (depth >= cfg.maxAggNesting) ? 0.0 : 0.20;
    double pStruct = (depth >= cfg.maxAggNesting) ? 0.0 : 0.15;
    double pPtr = (depth >= cfg.maxPtrDepth) ? 0.0 : 0.15;

    // Renormalize
    double total = pScalar + pArray + pStruct + pPtr;
    if (total <= 0.0) {
      // Only scalar left
      return genScalarType(rng, cfg.enableFp);
    }

    std::uniform_real_distribution<double> prob(0.0, total);
    double r = prob(rng);

    if (r < pScalar) {
      return genScalarType(rng, cfg.enableFp);
    }
    r -= pScalar;
    if (r < pArray) {
      // Array: [N] elem — elem is recursively generated with depth+1
      std::uniform_int_distribution<int> szd(1, std::max(1, cfg.maxAggElems));
      uint64_t sz = (uint64_t) szd(rng);
      TypePtr elem = genRandomType(rng, cfg, depth + 1);
      ArrayType at;
      at.size = sz;
      at.elem = elem;
      return std::make_shared<Type>(Type{at, {}});
    }
    r -= pArray;
    if (r < pStruct) {
      // Struct: return a placeholder StructType — the name will be filled in
      // by VarCatalogue. We just mark it as struct-typed here; the actual struct
      // declaration is created during catalogue generation.
      // For now, generate a sentinel StructType with an empty name.
      StructType st;
      st.name = GlobalId{"@_pending_struct", {}};
      return std::make_shared<Type>(Type{st, {}});
    }
    // Pointer
    TypePtr pointee = genRandomType(rng, cfg, depth + 1);
    // Pointers can only point to scalar or another pointer (SymIR v0.2.0 spec)
    // If the generated pointee is agg, fall back to scalar
    if (isAggType(pointee)) {
      pointee = genScalarType(rng, cfg.enableFp);
    }
    PtrType pt;
    pt.pointee = pointee;
    return std::make_shared<Type>(Type{pt, {}});
  }

} // namespace symir::reify
