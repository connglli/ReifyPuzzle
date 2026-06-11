#pragma once

#include <cstdint>
#include <optional>
#include "ast/ast.hpp"

namespace refractir {

  struct TypeUtils {
    /**
     * Returns the bitwidth of the given type if it is an integer type.
     * Returns std::nullopt for floats, pointers, vectors, aggregates.
     */
    static std::optional<std::uint32_t> getIntBitWidth(const TypePtr &t);

    /**
     * Returns the bitwidth of the given type if it is a float type.
     * Returns std::nullopt for integers, pointers, vectors, aggregates.
     */
    static std::optional<std::uint32_t> getFloatBitWidth(const TypePtr &t);

    /**
     * Returns the bitwidth of any scalar type (integer or float).
     *   i32 → 32, i64 → 64, iN → N, f32 → 32, f64 → 64.
     * Returns std::nullopt for pointers, vectors, aggregates.
     */
    static std::optional<std::uint32_t> getScalarBitWidth(const TypePtr &t);

    /**
     * Returns the total bitwidth of a vector type: N * scalarBitWidth(elem).
     * Returns std::nullopt for non-vector types or if the element type
     * is not a scalar (which the typechecker prevents).
     */
    static std::optional<std::uint32_t> getVectorBitWidth(const TypePtr &t);

    /**
     * Returns the bitwidth of any scalar or vector type.
     * Equivalent to: getScalarBitWidth(t) || getVectorBitWidth(t).
     * Returns std::nullopt for pointers, structs, and arrays.
     */
    static std::optional<std::uint32_t> getBitWidth(const TypePtr &t);

    /**
     * Checks if two types are structurally equal.
     */
    static bool areTypesEqual(const TypePtr &a, const TypePtr &b);

    /**
     * Casts to ArrayType if possible, otherwise returns nullptr.
     */
    static const ArrayType *asArray(const TypePtr &t);

    /**
     * Casts to StructType if possible, otherwise returns nullptr.
     */
    static const StructType *asStruct(const TypePtr &t);

    /**
     * Returns true if the type is an array type.
     */
    static bool isArray(const TypePtr &t);

    /**
     * Returns true if the type is a struct type.
     */
    static bool isStruct(const TypePtr &t);

    /**
     * [v0.2.1] Casts to VecType if possible, otherwise returns nullptr.
     */
    static const VecType *asVec(const TypePtr &t);

    /**
     * [v0.2.1] True iff the type is a vector type `<N> T`.
     */
    static bool isVec(const TypePtr &t);
  };

} // namespace refractir
