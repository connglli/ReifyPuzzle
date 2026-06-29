#include <cstdint>
#include <stdexcept>
#include "interp/interpreter.hpp"

namespace refractir {

  // ---- Memory helpers ----

  std::uint64_t Interpreter::sizeofType(const TypePtr &t) const {
    if (!t)
      return 8;
    if (auto it = std::get_if<IntType>(&t->v)) {
      uint32_t bits = it->bits.value_or(it->kind == IntType::Kind::I32 ? 32 : 64);
      return (bits + 7) / 8;
    }
    if (auto ft = std::get_if<FloatType>(&t->v)) {
      return ft->kind == FloatType::Kind::F32 ? 4 : 8;
    }
    if (std::holds_alternative<PtrType>(t->v))
      return 8;
    if (auto at = std::get_if<ArrayType>(&t->v)) {
      return at->size * sizeofType(at->elem);
    }
    if (auto st = std::get_if<StructType>(&t->v)) {
      auto it = structs_.find(st->name.name);
      if (it == structs_.end())
        return 8;
      uint64_t total = 0;
      for (const auto &f: it->second->fields)
        total += sizeofType(f.type);
      return total;
    }
    return 8;
  }

  TypePtr Interpreter::getCellTypeAtOffset(TypePtr t, std::uint64_t offset) const {
    if (!t)
      return nullptr;
    if (auto at = std::get_if<ArrayType>(&t->v)) {
      uint64_t elemSz = sizeofType(at->elem);
      if (elemSz == 0)
        return at->elem;
      uint64_t subOff = offset % elemSz;
      return getCellTypeAtOffset(at->elem, subOff);
    }
    if (auto st = std::get_if<StructType>(&t->v)) {
      auto sit = structs_.find(st->name.name);
      if (sit != structs_.end()) {
        uint64_t off = 0;
        for (const auto &f: sit->second->fields) {
          uint64_t fSize = sizeofType(f.type);
          if (offset >= off && offset < off + fSize) {
            return getCellTypeAtOffset(f.type, offset - off);
          }
          off += fSize;
        }
      }
    }
    return t;
  }

  // Byte offset of named field within struct s (sequential layout, no padding).
  std::uint64_t Interpreter::fieldOffset(const StructDecl &s, const std::string &fieldName) const {
    uint64_t offset = 0;
    for (const auto &f: s.fields) {
      if (f.name == fieldName)
        return offset;
      offset += sizeofType(f.type);
    }
    throw std::runtime_error("Internal: field '" + fieldName + "' not found in struct");
  }
} // namespace refractir
