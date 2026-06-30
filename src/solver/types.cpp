#include "analysis/type_utils.hpp"
#include "solver/solver.hpp"

namespace refractir {

  TypePtr SymbolicExecutor::resolveLValueType(const LValue &lv) const {
    if (!currentFun_)
      throw std::runtime_error("resolveLValueType: no active FunDecl");
    const std::string baseName = lv.base.name;
    TypePtr cur;
    for (const auto &l: currentFun_->lets) {
      if (l.name.name == baseName) {
        cur = l.type;
        break;
      }
    }
    if (!cur) {
      for (const auto &p: currentFun_->params) {
        if (p.name.name == baseName) {
          cur = p.type;
          break;
        }
      }
    }
    if (!cur)
      throw std::runtime_error("resolveLValueType: base local not found: " + baseName);

    for (const auto &acc: lv.accesses) {
      if (auto ai = std::get_if<AccessIndex>(&acc)) {
        (void) ai;
        if (auto at = std::get_if<ArrayType>(&cur->v)) {
          cur = at->elem;
        } else if (auto vt = std::get_if<VecType>(&cur->v)) {
          cur = vt->elem;
        } else {
          throw std::runtime_error("resolveLValueType: indexing non-array/vector type");
        }
      } else if (auto af = std::get_if<AccessField>(&acc)) {
        if (auto st = std::get_if<StructType>(&cur->v)) {
          auto sIt = structs_.find(st->name.name);
          if (sIt == structs_.end())
            throw std::runtime_error("resolveLValueType: unknown struct type: " + st->name.name);
          bool found = false;
          for (const auto &f: sIt->second->fields) {
            if (f.name == af->field) {
              cur = f.type;
              found = true;
              break;
            }
          }
          if (!found)
            throw std::runtime_error("resolveLValueType: field not found in struct: " + af->field);
        } else {
          throw std::runtime_error("resolveLValueType: field access on non-struct type");
        }
      }
    }
    return cur;
  }

  std::string SymbolicExecutor::buildLValueKey(const LValue &lv) const {
    std::string key = lv.base.name;
    for (const auto &acc: lv.accesses) {
      if (auto af = std::get_if<AccessField>(&acc)) {
        key += "." + af->field;
      } else {
        return {};
      }
    }
    return key;
  }

  TypePtr SymbolicExecutor::resolveExprType(const Expr &e) const {
    if (!currentFun_)
      return nullptr;
    return resolveAtomType(e.first);
  }

  TypePtr SymbolicExecutor::resolveAtomType(const Atom &a) const {
    return std::visit(
        [&](auto &&arg) -> TypePtr {
          using T = std::decay_t<decltype(arg)>;
          if constexpr (std::is_same_v<T, RValueAtom>) {
            return resolveLValueType(arg.rval);
          } else if constexpr (std::is_same_v<T, AddrAtom>) {
            auto pointeeTy = resolveLValueType(arg.lv);
            return std::make_shared<Type>(PtrType{pointeeTy, SourceSpan{}});
          } else if constexpr (std::is_same_v<T, PtrIndexAtom>) {
            auto srcTy = resolveLValueType(arg.rval);
            if (srcTy) {
              if (auto pt = std::get_if<PtrType>(&srcTy->v)) {
                if (auto at = std::get_if<ArrayType>(&pt->pointee->v)) {
                  return std::make_shared<Type>(PtrType{at->elem, SourceSpan{}});
                }
              }
            }
            return nullptr;
          } else if constexpr (std::is_same_v<T, PtrFieldAtom>) {
            auto srcTy = resolveLValueType(arg.rval);
            if (srcTy) {
              if (auto pt = std::get_if<PtrType>(&srcTy->v)) {
                if (auto st = std::get_if<StructType>(&pt->pointee->v)) {
                  auto sIt = structs_.find(st->name.name);
                  if (sIt != structs_.end()) {
                    for (const auto &f: sIt->second->fields) {
                      if (f.name == arg.field) {
                        return std::make_shared<Type>(PtrType{f.type, SourceSpan{}});
                      }
                    }
                  }
                }
              }
            }
            return nullptr;
          } else if constexpr (std::is_same_v<T, LoadAtom>) {
            auto srcTy = resolveLValueType(arg.rval);
            if (srcTy) {
              if (auto pt = std::get_if<PtrType>(&srcTy->v)) {
                return pt->pointee;
              }
            }
            return nullptr;
          } else if constexpr (std::is_same_v<T, SelectAtom>) {
            return resolveSelectValType(arg.vtrue);
          } else if constexpr (std::is_same_v<T, CastAtom>) {
            return arg.dstType;
          } else {
            if constexpr (std::is_same_v<T, CoefAtom>) {
              if (std::holds_alternative<NullLit>(arg.coef)) {
                return std::make_shared<Type>(PtrType{nullptr, SourceSpan{}});
              }
            }
            return nullptr;
          }
        },
        a.v
    );
  }

  TypePtr SymbolicExecutor::resolveSelectValType(const SelectVal &sv) const {
    if (auto rv = std::get_if<RValue>(&sv)) {
      return resolveLValueType(*rv);
    }
    auto coef = std::get<Coef>(sv);
    if (std::holds_alternative<NullLit>(coef)) {
      return std::make_shared<Type>(PtrType{nullptr, SourceSpan{}});
    }
    return nullptr;
  }
} // namespace refractir
