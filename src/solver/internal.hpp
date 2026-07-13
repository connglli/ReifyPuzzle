#pragma once

// Solver-private helpers shared across the symbolic executor's translation
// units. Kept inline in this detail header (not a separate .cpp) so each
// solver TU that needs them can pull them in without an extra object file.
// Not part of the public solver interface — internal to src/solver.
//
// The pointer-tag / size helpers (tagOfLocal, sizeofTagUnits,
// fieldOffsetTagUnits, typeMatch) underpin the provenance encoding and are
// candidates to fold into the Provenance collaborator later.

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include "ast/ast.hpp"
#include "solver/smt.hpp"

namespace refractir {

  // Pointers are encoded as 64-bit BV tags identifying the addressed local.
  // Tag 0 is reserved for null. Tags are derived deterministically from the
  // local name via FNV-1a so they remain stable across solver invocations.
  inline constexpr uint32_t kPtrBits = 64;

  inline uint64_t tagOfLocal(const std::string &name) {
    uint64_t h = 1469598103934665603ULL; // FNV-1a 64-bit offset basis
    for (unsigned char c: name) {
      h ^= c;
      h *= 1099511628211ULL; // FNV-1a 64-bit prime
    }
    if (h == 0)
      h = 1; // never collide with null
    return h;
  }

  // Map RelOp to the appropriate SMT comparison kind, choosing BV or FP
  // variants based on isFp.
  inline smt::Kind relOpToSmtKind(RelOp op, bool isFp) {
    switch (op) {
      case RelOp::EQ:
        return smt::Kind::EQUAL;
      case RelOp::NE:
        return smt::Kind::DISTINCT;
      case RelOp::LT:
        return isFp ? smt::Kind::FP_LT : smt::Kind::BV_SLT;
      case RelOp::LE:
        return isFp ? smt::Kind::FP_LEQ : smt::Kind::BV_SLE;
      case RelOp::GT:
        return isFp ? smt::Kind::FP_GT : smt::Kind::BV_SGT;
      case RelOp::GE:
        return isFp ? smt::Kind::FP_GEQ : smt::Kind::BV_SGE;
    }
    return smt::Kind::EQUAL;
  }

  // Size of a type measured in BV-tag units (one unit per scalar leaf).
  // Used by the pointer encoding so that:
  //   * `addr %arr[k]` on `[N] T` advances by `k * sizeofTagUnits(T)`
  //   * pointer arithmetic on `ptr T` scales by `sizeofTagUnits(T)`
  //   * `ptrfield`/`ptrindex` adds the right offset for nested aggregates
  inline std::uint64_t sizeofTagUnits(
      const TypePtr &t, const std::unordered_map<std::string, const StructDecl *> &structs
  ) {
    if (!t)
      return 1;
    if (auto at = std::get_if<ArrayType>(&t->v))
      return at->size * sizeofTagUnits(at->elem, structs);
    if (auto st = std::get_if<StructType>(&t->v)) {
      auto sIt = structs.find(st->name.name);
      if (sIt == structs.end())
        return 1;
      std::uint64_t sum = 0;
      for (const auto &f: sIt->second->fields)
        sum += sizeofTagUnits(f.type, structs);
      return sum;
    }
    return 1; // scalar / ptr / vec
  }

  // Byte offset (in tag units) of the named struct field.
  inline std::uint64_t fieldOffsetTagUnits(
      const StructDecl &s, const std::string &field,
      const std::unordered_map<std::string, const StructDecl *> &structs
  ) {
    std::uint64_t off = 0;
    for (const auto &f: s.fields) {
      if (f.name == field)
        return off;
      off += sizeofTagUnits(f.type, structs);
    }
    return off;
  }

  // Compare RefractIR types for structural equality at the level we care about
  // (matters when enumerating candidate ptr targets in load/store dispatch).
  inline bool typeMatch(const TypePtr &a, const TypePtr &b) {
    if (!a || !b)
      return a == b;
    if (a->v.index() != b->v.index())
      return false;
    if (auto pa = std::get_if<IntType>(&a->v)) {
      auto pb = std::get_if<IntType>(&b->v);
      auto width = [](const IntType &t) -> uint32_t {
        switch (t.kind) {
          case IntType::Kind::I32:
            return 32;
          case IntType::Kind::I64:
            return 64;
          case IntType::Kind::ICustom:
            return t.bits.value_or(32);
        }
        return 32;
      };
      return width(*pa) == width(*pb);
    }
    if (auto pa = std::get_if<FloatType>(&a->v)) {
      return pa->kind == std::get<FloatType>(b->v).kind;
    }
    if (auto pa = std::get_if<PtrType>(&a->v)) {
      return typeMatch(pa->pointee, std::get<PtrType>(b->v).pointee);
    }
    if (auto pa = std::get_if<VecType>(&a->v)) {
      auto pb = std::get_if<VecType>(&b->v);
      return pa->size == pb->size && typeMatch(pa->elem, pb->elem);
    }
    // Aggregate types as pointees are not supported in current solver dispatch.
    return false;
  }

  // Assert that a floating-point term is finite (not +/-inf, not NaN) by
  // pushing the guards onto the path condition.
  inline void assertFPFinite(smt::Term t, smt::ISolver &solver, std::vector<smt::Term> &ub) {
    auto notInf = solver.make_term(smt::Kind::NOT, {solver.make_term(smt::Kind::FP_IS_INF, {t})});
    auto notNaN = solver.make_term(smt::Kind::NOT, {solver.make_term(smt::Kind::FP_IS_NAN, {t})});
    ub.push_back(notInf);
    ub.push_back(notNaN);
  }

} // namespace refractir
