#include <cmath>
#include <sstream>
#include <stdexcept>
#include "analysis/type_utils.hpp"
#include "ast/ast.hpp"
#include "internal.hpp"
#include "interp/interpreter.hpp"

namespace refractir {

  std::string Interpreter::rvToString(const RuntimeValue &rv) const {
    switch (rv.kind) {
      case RuntimeValue::Kind::Int:
        return std::to_string(rv.intVal);
      case RuntimeValue::Kind::Float:
        return std::to_string(rv.floatVal);
      case RuntimeValue::Kind::Undef:
        return "undef";
      case RuntimeValue::Kind::Array:
        return "[...]";
      case RuntimeValue::Kind::Struct:
        return "{...}";
      case RuntimeValue::Kind::Ptr:
        return "ptr(0x" + std::to_string(rv.ptrVal) + ")";
      case RuntimeValue::Kind::Vec: {
        std::string s = "<";
        for (size_t i = 0; i < rv.arrayVal.size(); ++i) {
          if (i)
            s += ", ";
          s += rvToString(rv.arrayVal[i]);
        }
        s += ">";
        return s;
      }
    }
    return "?";
  }

  RuntimeValue Interpreter::makeUndef(const TypePtr &t) {
    RuntimeValue res;
    if (auto vt = TypeUtils::asVec(t)) {
      // [v0.2.1] Undef vector: every lane is undef. A subsequent lane
      // write produces a defined value at that lane; remaining lanes
      // stay undef until a whole-vector copy assigns them (rule 22).
      res.kind = RuntimeValue::Kind::Vec;
      for (size_t i = 0; i < vt->size; ++i)
        res.arrayVal.push_back(makeUndef(vt->elem));
    } else if (auto at = TypeUtils::asArray(t)) {
      res.kind = RuntimeValue::Kind::Array;
      for (size_t i = 0; i < at->size; ++i)
        res.arrayVal.push_back(makeUndef(at->elem));
    } else if (auto st = TypeUtils::asStruct(t)) {
      res.kind = RuntimeValue::Kind::Struct;
      auto it = typeLayout_.structs().find(st->name.name);
      if (it != typeLayout_.structs().end()) {
        for (const auto &f: it->second->fields)
          res.structVal[f.name] = makeUndef(f.type);
      }
    } else {
      auto bits = TypeUtils::getIntBitWidth(t);
      if (bits) {
        res.kind = RuntimeValue::Kind::Undef;
        res.bits = *bits;
      } else if (t && std::holds_alternative<FloatType>(t->v)) {
        res.kind = RuntimeValue::Kind::Undef;
        res.bits = (std::get<FloatType>(t->v).kind == FloatType::Kind::F32) ? 32 : 64;
      } else if (t && std::holds_alternative<PtrType>(t->v)) {
        res.kind = RuntimeValue::Kind::Undef;
        res.bits = 64;
      } else {
        res.kind = RuntimeValue::Kind::Undef;
        res.bits = 64;
      }
    }
    return res;
  }

  RuntimeValue Interpreter::broadcast(const TypePtr &t, const RuntimeValue &v) {
    if (v.kind == RuntimeValue::Kind::Vec || v.kind == RuntimeValue::Kind::Array ||
        v.kind == RuntimeValue::Kind::Struct) {
      return v;
    }
    if (auto vt = TypeUtils::asVec(t)) {
      // [v0.2.1] Broadcast init for vector: each lane gets a copy of `v`
      // canonicalized to the lane scalar type.
      RuntimeValue res;
      res.kind = RuntimeValue::Kind::Vec;
      for (size_t i = 0; i < vt->size; ++i)
        res.arrayVal.push_back(broadcast(vt->elem, v));
      return res;
    } else if (auto at = TypeUtils::asArray(t)) {
      RuntimeValue res;
      res.kind = RuntimeValue::Kind::Array;
      for (size_t i = 0; i < at->size; ++i)
        res.arrayVal.push_back(broadcast(at->elem, v));
      return res;
    } else if (auto st = TypeUtils::asStruct(t)) {
      RuntimeValue res;
      res.kind = RuntimeValue::Kind::Struct;
      auto it = typeLayout_.structs().find(st->name.name);
      if (it != typeLayout_.structs().end()) {
        for (const auto &f: it->second->fields)
          res.structVal[f.name] = broadcast(f.type, v);
      }
      return res;
    } else {
      RuntimeValue res = v;
      auto bits = TypeUtils::getIntBitWidth(t);
      if (bits) {
        res.bits = *bits;
        res.intVal = canonicalize(res.intVal, res.bits);
      } else if (t && std::holds_alternative<FloatType>(t->v)) {
        res.bits = (std::get<FloatType>(t->v).kind == FloatType::Kind::F32) ? 32 : 64;
      }
      return res;
    }
  }

  RuntimeValue Interpreter::evalInit(const InitVal &iv, const TypePtr &t, const Store &store) {
    if (iv.kind == InitVal::Kind::Undef)
      return makeUndef(t);

    if (iv.kind == InitVal::Kind::Null) {
      RuntimeValue rv;
      rv.kind = RuntimeValue::Kind::Ptr;
      rv.ptrVal = 0; // null = address 0
      rv.bits = 64;
      return rv;
    }

    if (iv.kind == InitVal::Kind::Aggregate) {
      const auto &elements = std::get<std::vector<InitValPtr>>(iv.value);
      if (auto vt = TypeUtils::asVec(t)) {
        // [v0.2.1] Brace init for vector: each lane init is a scalar.
        RuntimeValue res;
        res.kind = RuntimeValue::Kind::Vec;
        for (size_t i = 0; i < elements.size(); ++i)
          res.arrayVal.push_back(evalInit(*elements[i], vt->elem, store));
        return res;
      } else if (auto at = TypeUtils::asArray(t)) {
        RuntimeValue res;
        res.kind = RuntimeValue::Kind::Array;
        for (size_t i = 0; i < elements.size(); ++i)
          res.arrayVal.push_back(evalInit(*elements[i], at->elem, store));
        return res;
      } else if (auto st = TypeUtils::asStruct(t)) {
        RuntimeValue res;
        res.kind = RuntimeValue::Kind::Struct;
        auto sit = typeLayout_.structs().find(st->name.name);
        if (sit != typeLayout_.structs().end()) {
          for (size_t i = 0; i < sit->second->fields.size(); ++i) {
            res.structVal[sit->second->fields[i].name] =
                evalInit(*elements[i], sit->second->fields[i].type, store);
          }
        }
        return res;
      }
    }

    // Scalar
    RuntimeValue v;
    if (iv.kind == InitVal::Kind::Int) {
      v.kind = RuntimeValue::Kind::Int;
      v.intVal = std::get<IntLit>(iv.value).value;
    } else if (iv.kind == InitVal::Kind::Float) {
      v.kind = RuntimeValue::Kind::Float;
      v.floatVal = std::get<FloatLit>(iv.value).value;
    } else if (iv.kind == InitVal::Kind::Sym) {
      v = store.at(std::get<SymId>(iv.value).name);
    } else if (iv.kind == InitVal::Kind::Atom) {
      // [v0.2.1] §3.4.2 atom-form init — evaluate the atom against the
      // partially-built store. Inits are processed in declaration order
      // so any local the atom references must already be in the store.
      v = evalAtom(*std::get<AtomPtr>(iv.value), store);
    } else {
      v = store.at(std::get<LocalId>(iv.value).name);
    }

    if (v.kind == RuntimeValue::Kind::Int) {
      v.bits = TypeUtils::getIntBitWidth(t).value_or(64);
      v.intVal = canonicalize(v.intVal, v.bits);
    } else if (v.kind == RuntimeValue::Kind::Float) {
      v.bits = (t && std::holds_alternative<FloatType>(t->v))
                   ? (std::get<FloatType>(t->v).kind == FloatType::Kind::F32 ? 32 : 64)
                   : 64;
      // SPEC §6.4: an init value for an f32 local must take its f32 precision.
      // Without this, a non-exactly-representable literal like
      // `let %a: f32 = 16777217.0` keeps the f64 image (exact 16777217.0)
      // while claiming bits=32, so later arithmetic diverges from the lowered
      // C/WASM (where the literal would round to 16777216.0 under RNE).
      if (v.bits == 32)
        v.floatVal = static_cast<double>(static_cast<float>(v.floatVal));
    }

    if (TypeUtils::asArray(t) || TypeUtils::asStruct(t) || TypeUtils::asVec(t)) {
      return broadcast(t, v);
    }
    return v;
  }
} // namespace refractir
