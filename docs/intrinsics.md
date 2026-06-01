# SymIR Standard Intrinsics

> **Spec reference**: §12 of [SPEC_v0.2.2.md](./SPEC_v0.2.2.md)
> **Implementation files** (one per tool — add new intrinsics to all four):
>
> | Tool | File |
> |------|------|
> | Interpreter | `src/interp/intrinsics.cpp` |
> | Solver (SMT) | `src/solver/intrinsics.cpp` |
> | Compiler → C | `src/backend/intrinsics_c.cpp` |
> | Compiler → WASM | `src/backend/intrinsics_wasm.cpp` |

---

## Overview

An **intrinsic** is a built-in function whose semantics are defined entirely by
the SymIR toolchain — not delegated to the target language. Each intrinsic has:

- A hard-coded implementation in the **interpreter** (`symiri`)
- A fixed SMT encoding in the **solver** (`symirsolve`)
- A widening-and-mask lowering rule in the **compiler** (`symirc` → C and WASM)

Intrinsics are declared in source with the `intrinsic` keyword before use:

```text
intrinsic @abs(%x: i32) : i32;
```

The declaration says nothing about the body — the toolchain provides it. Every
intrinsic is declared once per concrete bit-width the program uses (e.g. `i32`
for 32-bit `@abs`). The same generic encoding applies regardless of the width
`N`.

### Declaration syntax

```text
IntrinsicDecl := "intrinsic" GlobalId "(" ParamList? ")" ":" Type ";" ;
```

All current intrinsics are typed over `iN` for any integer width `N ≥ 1`.
Floating-point intrinsics are **not** supported in v0.2.2.

### Widening-and-mask lowering (§11.5)

The compiler maps `iN` to the smallest machine width `W` that fits:

| `N` | `W` (C / WASM) |
|-----|----------------|
| 1–8 | 8 |
| 9–16 | 16 |
| 17–32 | 32 |
| 33–64 | 64 |

Each helper widens operands to `W`, performs the operation at `W`-bit width,
then sign-extends / masks the result back to `N` bits.

---

## 12.1 Arithmetic intrinsics

### `@abs` — signed absolute value

```text
intrinsic @abs(%x: iN) : iN;
```

Returns the absolute value of `%x`.

**UB conditions**:
- `%x == INT_MIN_N` — the result would be `−INT_MIN_N`, which is not
  representable in a signed `iN`. This is consistent with rule 4 in §7.1
  (signed integer overflow is UB).

**Semantics per tool**:

| Tool | Behaviour |
|------|-----------|
| Interpreter | Assert `x != INT_MIN_N` (aborts path if violated); return `x < 0 ? -x : x`. |
| Solver | Add `x ≠ INT_MIN_N` to path condition; return `ite(bvsge(x, 0), x, bvneg(x))`. |
| C codegen | `if (a0 == INT_MIN_N) __builtin_trap(); return a0 < 0 ? -a0 : a0;` (widened to `intW_t`). |
| WASM codegen | `if (a0 == INT_MIN_N) unreachable;` then `select` between `0 - a0` and `a0` based on `lt_s`. |

**Example**:

```text
intrinsic @abs(%x: i32) : i32;

fun @main() : i32 {
  let %n: i32 = -42;
  let mut %r: i32 = 0;
^entry:
  %r = call @abs(%n);   // %r == 42
  ret %r;
}
```

**Synthesis example** — find `%?x` such that `@abs(%?x) == 5`:

```text
intrinsic @abs(%x: i32) : i32;
fun @main() : i32 {
  sym %?x: value i32 in [-100, 100];
  let mut %r: i32 = 0;
^entry:
  %r = call @abs(%?x);
  require %r == 5, "abs(?x) must equal 5";
  br ^exit;
^exit:
  ret %?x;  // solver returns 5 or -5
}
```

---

### `@min`, `@max` — signed minimum / maximum

```text
intrinsic @min(%a: iN, %b: iN) : iN;
intrinsic @max(%a: iN, %b: iN) : iN;
```

Return the signed minimum or maximum of two `iN` values. No UB conditions
(the result is always representable when the inputs are).

**Semantics per tool**:

| Tool | `@min` | `@max` |
|------|--------|--------|
| Interpreter | `a < b ? a : b` | `a > b ? a : b` |
| Solver | `ite(bvsle(a, b), a, b)` | `ite(bvsge(a, b), a, b)` |
| C codegen | `a0 < a1 ? a0 : a1` (widened to `intW_t`) | `a0 > a1 ? a0 : a1` |
| WASM codegen | `select` with `lt_s` | `select` with `gt_s` |

**Example**:

```text
intrinsic @min(%a: i32, %b: i32) : i32;
intrinsic @max(%a: i32, %b: i32) : i32;

fun @clamp(%v: i32, %lo: i32, %hi: i32) : i32 {
  let mut %r: i32 = 0;
^entry:
  %r = call @max(%v, %lo);
  %r = call @min(%r, %hi);
  ret %r;
}
```

---

## 12.2 Bit-counting intrinsics

### `@clz` — count leading zeros

```text
intrinsic @clz(%x: iN) : iN;
```

Returns the number of leading zero bits in `%x`, counting from bit `N−1`
(most significant) down to bit 0.

**UB conditions**:
- `%x == 0` — following C/C++ `__builtin_clz` semantics, zero input is UB.
  Callers must ensure `%x != 0` on the chosen execution path.

**Result range**: `[0, N−1]` (since `%x != 0`, at least one bit is set).

**Semantics per tool**:

| Tool | Behaviour |
|------|-----------|
| Interpreter | Loop from bit `N−1` down; count zeros before the first `1`. Software fallback for any `N`. |
| Solver | Add `x ≠ 0` to PC; build an ITE chain: iterate `i = 0 … N−1`, emit `ite(bit(N−1−i) == 1, i, result)` from the back. |
| C codegen | `u = maskU(a0); if (u==0) __builtin_trap(); __builtin_clz[ll](u) − (W−N)` (widened to `uintW_t`). |
| WASM codegen | Mask → `local.tee`; eqz guard → `unreachable`; then `iW.clz − (W−N)`. |

> **Why subtract `W−N`?** The builtin counts zeros in a `W`-bit word. The
> high `W−N` bits of the widened value are always zero (they were masked
> off), so they must be excluded from the count.

**Example**:

```text
intrinsic @clz(%x: i32) : i32;

fun @main() : i32 {
  let %n: i32 = 1;     // binary: 000...001
  let mut %r: i32 = 0;
^entry:
  %r = call @clz(%n);  // %r == 31 (for i32)
  ret %r;
}
```

---

### `@ctz` — count trailing zeros

```text
intrinsic @ctz(%x: iN) : iN;
```

Returns the number of trailing zero bits in `%x`, counting from bit 0
(least significant) up to bit `N−1`.

**UB conditions**:
- `%x == 0` — following C/C++ `__builtin_ctz` semantics, zero input is UB.

**Result range**: `[0, N−1]` (since `%x != 0`).

**Semantics per tool**:

| Tool | Behaviour |
|------|-----------|
| Interpreter | Loop from bit 0 up; count zeros before the first `1`. |
| Solver | Add `x ≠ 0` to PC; build an ITE chain: iterate `i = N−1 … 0`, emit `ite(bit(i) == 1, i, result)` from the back. |
| C codegen | `u = maskU(a0); if (u==0) __builtin_trap(); __builtin_ctz[ll](u)` (no bias needed — low bits are unaffected by widening). |
| WASM codegen | Mask → `local.tee`; eqz guard → `unreachable`; then `iW.ctz`. |

**Example**:

```text
intrinsic @ctz(%x: i32) : i32;

fun @main() : i32 {
  let %n: i32 = 12;    // binary: 000...01100
  let mut %r: i32 = 0;
^entry:
  %r = call @ctz(%n);  // %r == 2
  ret %r;
}
```

---

### `@popcount` — population count

```text
intrinsic @popcount(%x: iN) : iN;
```

Returns the number of 1 bits in `%x` (also called Hamming weight).

**UB conditions**:
- **Result overflow at very narrow widths.** For a signed `iN`, the result
  must fit in `iN`. E.g. for `i2` the signed maximum is 1 — `@popcount(3)`
  (result 2) is UB because 2 > `INT_MAX_i2 = 1`. This is rule 25 in §7.7.
  For `N ≥ 7` this UB is unreachable because `popcount ≤ N ≤ INT_MAX_N`.

**Result range**: `[0, N]` (or `[0, INT_MAX_N]` for narrow widths).

**Semantics per tool**:

| Tool | Behaviour |
|------|-----------|
| Interpreter | `__builtin_popcountll(maskU(x))`; UB-check that result ≤ `INT_MAX_N`. |
| Solver | Build `Σ zext(bit_k(x), N)` via `bvadd` over `N` one-bit extracts. No explicit UB guard — the bit-vector sum is always within `[0, N]`. |
| C codegen | `u = maskU(a0); __builtin_popcount[ll](u)` (widened to `uintW_t`). |
| WASM codegen | Mask input then `iW.popcnt`. |

**Example**:

```text
intrinsic @popcount(%x: i32) : i32;

fun @main() : i32 {
  let %n: i32 = 7;       // binary: 111
  let mut %r: i32 = 0;
^entry:
  %r = call @popcount(%n); // %r == 3
  ret %r;
}
```

---

## Quick-reference table

| Intrinsic | Signature | UB conditions | Result range |
|-----------|-----------|---------------|--------------|
| `@abs` | `(iN) → iN` | `x == INT_MIN_N` | `[0, INT_MAX_N]` |
| `@min` | `(iN, iN) → iN` | — | `[INT_MIN_N, INT_MAX_N]` |
| `@max` | `(iN, iN) → iN` | — | `[INT_MIN_N, INT_MAX_N]` |
| `@clz` | `(iN) → iN` | `x == 0` | `[0, N−1]` |
| `@ctz` | `(iN) → iN` | `x == 0` | `[0, N−1]` |
| `@popcount` | `(iN) → iN` | result > `INT_MAX_N` (narrow `N` only) | `[0, N]` |

---

## Adding a new intrinsic

A new intrinsic must have all four pieces before it can be merged. The rule
from §12.3: *"Delegate to the target" is not acceptable — SymIR owns the
semantics.*

1. **Declare** its signature in the test / user program (`intrinsic @name(...) : T;`).
2. **Interpreter** (`src/interp/intrinsics.cpp`): add a branch in
   `Interpreter::callIntrinsic`. Throw `UndefinedBehaviorError` for every
   UB precondition.
3. **Solver** (`src/solver/intrinsics.cpp`): add a branch in
   `SymbolicExecutor::callBuiltinIntrinsicSMT`. Push UB guards onto `pc`
   using `solver.make_term(smt::Kind::DISTINCT, ...)`.
4. **C codegen** (`src/backend/intrinsics_c.cpp`): add a branch in
   `CBackend::emitIntrinsicHelper`. Use `__builtin_trap()` for UB paths.
5. **WASM codegen** (`src/backend/intrinsics_wasm.cpp`): add a branch in
   `WasmBackend::emitIntrinsicHelper`. Use `unreachable` for UB paths.
6. **Spec**: document the new intrinsic in §12 of `SPEC_v0.2.2.md` and update
   this file.
7. **Tests**: add tests in `test/interp/`, `test/compile/`, `test/solver/`,
   and `test/xval/`.

### What to specify

- Signature (over `iN` or other types)
- UB preconditions (which inputs make the result undefined)
- Result range (what values can be returned)
- SMT encoding (exact BV formula or ITE chain)
- Interpreter algorithm (concrete integer arithmetic)
- C and WASM lowering pattern (widening-and-mask or equivalent)

---

## Planned / not-yet-supported intrinsics

The following are explicitly out of scope for v0.2.2 (see §13):

- **Memory intrinsics** (`@memcpy`, `@memset`): require byte-level array
  reasoning with potentially symbolic sizes. Deferred.
- **Vector reductions** (`@reduce_add`, `@reduce_max`, …): horizontal
  lane-wise reductions. Planned for v0.2.3; use manual lane subscripts
  (`%v[0] + %v[1] + …`) in the meantime.
- **Floating-point intrinsics**: `f32`/`f64` variants of the above. The
  finite-only FP domain makes `@abs` trivially expressible as `fabs`, but
  the full SMT encoding for FP is deferred.
