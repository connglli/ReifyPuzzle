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

Most intrinsics are typed over `iN` for any integer width `N ≥ 1` —
input and return width agree, and the toolchain applies a uniform
widening-and-mask lowering. Two exceptions live in §12.4: **`@parity`
and `@is_pow2` return `i1`** regardless of input width, because they
are pure predicates. Their widening-and-mask rule treats the return
type as a fixed `i1` (lowered to `int8_t` in C, `i32` in WASM, `bv(1)`
in SMT) and is unrelated to the input width `N`.

Floating-point intrinsics are **not** supported in v0.2.2 baseline.
The P0 floating-point basic IEEE family is planned for a future v0.2.2
follow-up; see *Priority tiers* below.

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

## 12.3 Integer extras

These broaden the existing arithmetic family with solver-trivial
integer operations that programs reach for most often. All entries
are solver-★/◐ and target-★ on both backends.

### `@abs_diff` — absolute difference

```text
intrinsic @abs_diff(%a: iN, %b: iN) : iN;
```

Returns `|a − b|` interpreted as a non-negative signed `iN`.

**UB conditions**:
- `|a − b| > INT_MAX_N` — the absolute difference is not representable
  in signed `iN` (rule 25 of §7.7). Equivalently: the signed subtraction
  underlying the result would overflow.

**Result range**: `[0, INT_MAX_N]`.

| Tool | Behaviour |
|---|---|
| Interpreter | Compute `int64_t s = (int64_t)a − (int64_t)b` (always non-overflowing for `N ≤ 32`; uses `__int128` for `N == 64`). Trap if `\|s\| > INT_MAX_N`. Return `\|s\|`. |
| Solver | `r = ite(bvsge(a, b), bvsub(a, b), bvsub(b, a))`. Conjoin `bvsge(r, 0)` to `PC` (signed-comparison-only UB check). |
| C codegen | Widen both operands to `int64_t` (`__int128` for `N == 64`), subtract, take absolute value, trap if it exceeds `INT_MAX_N`, narrow back. No unsigned cast. |
| WASM codegen | Both differences computed via `iW.sub` (WASM signed/unsigned-agnostic bit-level subtraction), `select` on `iW.lt_s`, UB-check by sign-extending and comparing `lt_s 0`. |

### `@signum` — sign of a signed integer

```text
intrinsic @signum(%x: iN) : iN;
```

Returns `−1` if `x < 0`, `0` if `x == 0`, `+1` if `x > 0`. No UB.

**Result range**: `{−1, 0, +1}`.

| Tool | Behaviour |
|---|---|
| Interpreter | `(x > 0) − (x < 0)`. |
| Solver | `ite(bvslt(x, 0), bv(−1, N), ite(EQUAL(x, 0), bv(0, N), bv(1, N)))`. |
| C codegen | Same ternary, widened. |
| WASM codegen | Two nested `select` over `iW.lt_s` against `0`. |

### `@clamp` — signed clamp

```text
intrinsic @clamp(%v: iN, %lo: iN, %hi: iN) : iN;
```

Returns `v` clipped to `[lo, hi]` under signed ordering: `max(lo, min(v, hi))`.

**UB conditions**:
- `lo > hi` (signed). The clamp range must be non-empty (matches
  Rust's `iN::clamp`, which panics on inverted bounds).

**Result range**: `[lo, hi]`.

| Tool | Behaviour |
|---|---|
| Interpreter | Trap if `lo > hi`; otherwise `v < lo ? lo : (v > hi ? hi : v)`. |
| Solver | Conjoin `bvsle(lo, hi)` to `PC`; emit `ite(bvslt(v, lo), lo, ite(bvsgt(v, hi), hi, v))`. |
| C codegen | UB-trap on inverted bounds, then nested ternaries. |
| WASM codegen | UB-check (`lt_s`, `if; unreachable; end`) then two `select` on `lt_s` / `gt_s`. |

### `@midpoint` — signed midpoint (truncation toward zero)

```text
intrinsic @midpoint(%a: iN, %b: iN) : iN;
```

Returns `(a + b) / 2` with division **truncating toward zero**
(consistent with §2.5 — `midpoint(3, 0) == 1`, `midpoint(−3, 0) == −1`).
The mathematical midpoint of any two `iN` values is always representable
in `iN`, so this operation has no UB.

| Tool | Behaviour |
|---|---|
| Interpreter | Compute `(int64_t)a + (int64_t)b` to avoid `iN` overflow, divide by 2 with C-style truncation toward 0, sign-mask to `N` bits. |
| Solver | Sign-extend both operands to `bv(N+1)`, `bvadd`, `bvsdiv` by `2` (signed BV division truncates toward zero), extract low `N` bits. |
| C codegen | `int64_t s = (int64_t)a0 + (int64_t)a1; intW_t r = (intW_t)(s / 2);` — sum always fits in `i64` for `N ≤ 64`. |
| WASM codegen | `i64.extend_iW_s` both, `i64.add`, `i64.const 2`, `i64.div_s`, `iW.wrap_i64`, sign-mask. (For `N = 64`, use `i128` emulation via two-step: split into high/low, midpoint of halves; but `N ≤ 63` is the common case — handle `N = 64` via a separate path that adds with a carry detection.) |

---

## 12.4 Bit-manipulation

All entries are solver-★/◐. WASM has native ops for shifts and the
rotation primitives `@rotl`/`@rotr` use them directly; `@bswap` and
`@bitreverse` require small compositions on WASM.

### `@parity` — bit parity

```text
intrinsic @parity(%x: iN) : i1;
```

Returns `1` if `x` has an odd number of one-bits, else `0`. **The return
type is `i1`, not `iN`** — `@parity` and `@is_pow2` are the first
intrinsics whose return type is not parameterised by the input width.

No UB. Result range: `{0, 1}`.

| Tool | Behaviour |
|---|---|
| Interpreter | `__builtin_parityll(maskU(x)) & 1`, stored as a sign-extended `i1`. |
| Solver | XOR all `N` single-bit extracts of `x` together; the resulting `bv(1)` is the `i1` result. |
| C codegen | Widen, mask, `__builtin_parity[ll](u)`, store as `int8_t` (which is the C representation of `i1` in the widening-and-mask scheme). |
| WASM codegen | Mask, `iW.popcnt`, `iW.const 1`, `iW.and`. Convert to `i1` representation. |

### `@bswap` — byte swap

```text
intrinsic @bswap(%x: iN) : iN;
```

Reverses the byte order of `x`.

**Declaration restriction**: `@bswap(%x: iN) : iN` is only well-formed
when `N % 8 == 0`. The semantic checker rejects declarations with
`N` not a multiple of 8. For `N == 8` the operation is the identity
(included for uniformity).

No UB.

| Tool | Behaviour |
|---|---|
| Interpreter | Byte loop: extract bytes from the masked value and reassemble in reverse order. |
| Solver | `BV_CONCAT` of `N/8` byte-sized `BV_EXTRACT` chunks in reverse order. |
| C codegen | Widen + mask, emit `__builtin_bswap{16,32,64}` (for `N == 8` the helper returns `a0` unchanged). For `N` that doesn't match a native bswap (e.g. `i24` is excluded by the declaration rule), no case needed. |
| WASM codegen | No native byte-swap. Emit a sequence of `iW.shr_u` / `iW.and` / `iW.shl` / `iW.or` operations — one per byte, positioning each byte at its reversed offset. |

### `@bitreverse` — bit reversal

```text
intrinsic @bitreverse(%x: iN) : iN;
```

Reverses all `N` bits of `x` (bit `i` ↔ bit `N − 1 − i`). Defined for
all `N ≥ 1`. No UB.

| Tool | Behaviour |
|---|---|
| Interpreter | Bit loop: write the result bit-by-bit by extracting bit `i` of input and ORing it into position `N − 1 − i` of output. |
| Solver | `BV_CONCAT` of all `N` 1-bit `BV_EXTRACT` slices in reversed order. |
| C codegen | Software bit-loop (GCC has no `__builtin_bitreverse`). Clang's `__builtin_bitreverseN` is unused — the loop is short enough that the optimiser handles common widths. |
| WASM codegen | Software bit-loop via a `loop`/`br_if` block. |

### `@rotl`, `@rotr` — bitwise rotation

```text
intrinsic @rotl(%x: iN, %n: iN) : iN;
intrinsic @rotr(%x: iN, %n: iN) : iN;
```

Rotate `x` by `n` bit positions. `@rotl` shifts toward higher-order
bits; `@rotr` toward lower-order bits.

**UB conditions** (consistent with the overshift rule §7.1 rule 5):
- `n < 0` — negative rotation amount.
- `n >= N` — rotation amount must be in `[0, N)`.

Callers wanting Rust/WASM-style mod-`N` behaviour must mask `n`
themselves (`n & (N − 1)` for power-of-two `N`).

| Tool | Behaviour |
|---|---|
| Interpreter | Trap on out-of-range `n`; otherwise bit-level shift-and-or — extract the low `N` bits of `x`, shift left by `n` and right by `N − n` (bit-level, zero-fill), OR the results, mask back to `N` bits. |
| Solver | Conjoin `bvsge(n, 0) ∧ bvslt(n, N)` to `PC`; emit the rotation as `bvor(bvshl(x, n), bv_shr_logical(x, N − n))`. Logical shifts are used because rotation is a bit-permutation, not a signed-arithmetic shift. |
| C codegen | UB-trap, then a shift-and-or composition on the iN bit pattern using the backend's bit-level helpers (the same widening-and-mask vehicle that hosts iN values). |
| WASM codegen | UB-check (`iW.lt_s 0` or `iW.ge_s N`, `if; unreachable; end`); compose `iW.shl` and `iW.shr_u` (logical shift right is the bit-level primitive used by rotation, not a numeric reinterpretation). |

### `@is_pow2` — power-of-two predicate

```text
intrinsic @is_pow2(%x: iN) : i1;
```

Returns `1` if `x` is a positive power of two (`x > 0` and
`popcount(x) == 1`), else `0`. Like `@parity`, the return type is `i1`.

No UB. Result range: `{0, 1}`.

| Tool | Behaviour |
|---|---|
| Interpreter | `(x > 0) && ((x & (x − 1)) == 0)`. |
| Solver | `ite(bvslt(0, x) ∧ EQUAL(bvand(x, bvsub(x, 1)), bv(0, N)), bv(1, 1), bv(0, 1))`. |
| C codegen | Same boolean, widened, written as a single expression with `&&`. |
| WASM codegen | Compose `iW.gt_s 0` with `iW.sub`, `iW.and`, `iW.eqz`; AND the two via `iW.and`. |

### `@ilog2` — floor log base 2

```text
intrinsic @ilog2(%x: iN) : iN;
```

Returns `floor(log2(x))` for strictly positive `x` (the position of the
most significant set bit).

**UB conditions**:
- `x <= 0` (signed). `@ilog2` is only defined for strictly positive `x`.

**Result range**: `[0, N − 2]` (the sign bit must be zero, so the
highest possible MSB position is `N − 2`).

| Tool | Behaviour |
|---|---|
| Interpreter | Trap on `x <= 0`; otherwise `(N − 1) − __builtin_clzll(maskU(x))`. |
| Solver | Conjoin `bvslt(bv(0, N), x)` to `PC`; build the `@clz` ITE chain and return `(N − 1) − clz`. |
| C codegen | UB-trap; `(N − 1) − __builtin_clz[ll](u)` widened. |
| WASM codegen | UB-check (`iW.const 0; iW.le_s; if; unreachable; end`); mask, `iW.clz`, `iW.const (W − 1); iW.sub`. |

## 12.5 Integer overflow-aware family (v0.2.2 extra batch C)

Wrapping arithmetic computes `(op) mod 2^N` with no UB on overflow.
Saturating arithmetic clamps at `[INT_MIN_N, INT_MAX_N]`.  Euclidean
division differs from the C-style truncating division in §7.2 only when
the dividend's sign forces a negative remainder.  All members are
declared at one common `iN` width — input and result share the same
type.

The tuple-returning members of this family (`@checked_*`,
`@overflowing_*`) and the cross-width `@widening_mul` are slated for a
follow-up batch that introduces a multi-value return ABI; the scalar-
result subset shipped here exercises the same arithmetic primitives.

### `@wrapping_add`, `@wrapping_sub`, `@wrapping_mul`, `@wrapping_neg` — modular arithmetic

```text
intrinsic @wrapping_add(%a: iN, %b: iN) : iN;
intrinsic @wrapping_sub(%a: iN, %b: iN) : iN;
intrinsic @wrapping_mul(%a: iN, %b: iN) : iN;
intrinsic @wrapping_neg(%x: iN) : iN;
```

Compute the operation modulo `2^N` and sign-extend the low `N` bits
back into the declared `iN`.  None of these intrinsics raises UB on
overflow; `@wrapping_neg(INT_MIN_N) == INT_MIN_N` is the canonical
fixed point.

| Tool | Behaviour |
|---|---|
| Interpreter | Compute on `uint64_t`, narrow via the iN mask. |
| Solver | Direct `bvadd / bvsub / bvmul / bvneg` (modular by construction). |
| C codegen | `(uty)(ua op ub)` then `sextN`. |
| WASM codegen | `iW.{add,sub,mul,sub}`, then `sextN`. |

### `@wrapping_shl`, `@wrapping_shr` — modular shifts

```text
intrinsic @wrapping_shl(%x: iN, %n: iN) : iN;
intrinsic @wrapping_shr(%x: iN, %n: iN) : iN;
```

`@wrapping_shl` is a left shift mod `2^N`; `@wrapping_shr` is the
arithmetic right shift (preserves the sign bit).  Both require the
shift count to be in `[0, N)` — matching the OpAtom shift rule §7.1
rule 5.

**UB conditions**: `n < 0` or `n >= N`.

| Tool | Behaviour |
|---|---|
| Interpreter | `argSint` for the shift; `x << n` / `x >> n` on int64, then narrow. |
| Solver | Push `0 ≤ n < N` to `PC`; `bvshl` / `bvashr`. |
| C codegen | Trap on UB; shift the widened unsigned (shl) or sign-extended signed (shr) value. |
| WASM codegen | UB-check via two `lt_s` / `ge_s` traps; `iW.{shl,shr_s}`. |

### `@saturating_add`, `@saturating_sub`, `@saturating_mul`, `@saturating_neg` — clamp at iN bounds

```text
intrinsic @saturating_add(%a: iN, %b: iN) : iN;
intrinsic @saturating_sub(%a: iN, %b: iN) : iN;
intrinsic @saturating_mul(%a: iN, %b: iN) : iN;
intrinsic @saturating_neg(%x: iN) : iN;
```

Compute the true result over the integers and clamp to
`[INT_MIN_N, INT_MAX_N]`.  `@saturating_neg(INT_MIN_N) == INT_MAX_N` is
the only overflow case for the unary form.

| Tool | Behaviour |
|---|---|
| Interpreter | Widen to `int64` (or `__int128` for N == 64); compute true result; clamp. |
| Solver | `BV_S{ADD,SUB,MUL}_OVERFLOW` predicate plus `BV_NEG`; ITE-select the saturated bound from the sign of `a` (or `a XOR b` for mul). |
| C codegen | Widen to `int64` (N ≤ 32) / `__int128` (N == 64), clamp; narrow back. |
| WASM codegen | i64-widen + clamp for N ≤ 32; sign-bit identity overflow detection for N == 64. |

### `@div_euclid`, `@rem_euclid` — Euclidean division

```text
intrinsic @div_euclid(%a: iN, %b: iN) : iN;
intrinsic @rem_euclid(%a: iN, %b: iN) : iN;
```

The Euclidean quotient rounds toward `−∞` instead of toward 0; the
Euclidean remainder is always non-negative and strictly less than
`|b|`.  Equivalent to Rust's `i*::div_euclid` / `i*::rem_euclid`.

**UB conditions**:
- `b == 0` for either.
- `a == INT_MIN_N && b == -1`: the true quotient `−INT_MIN_N` overflows
  the iN range (identical to the C-style div / mod UB §7.2).

| Tool | Behaviour |
|---|---|
| Interpreter | Trunc div; if the trunc remainder is negative, step the quotient toward `−∞`. |
| Solver | Push `b ≠ 0` and `¬(a = INT_MIN ∧ b = −1)` to `PC`; combine `bvsdiv` / `bvsrem` with an ITE adjustment. |
| C codegen | Trap on UB; `q = a / b`, `r = a − q·b`, adjust if `r < 0`. |
| WASM codegen | Two unreachable-guarded checks; `iW.div_s` + `iW.mul / iW.sub`; conditional adjustment via `if/end`. |

### Example — overflow-aware accumulator

```text
intrinsic @saturating_add(%a: i16, %b: i16) : i16;
intrinsic @wrapping_mul(%a: i16, %b: i16) : i16;
intrinsic @rem_euclid(%a: i32, %b: i32) : i32;

fun @reduce(%xs: [4] i16, %m: i32) : i32 {
  let mut %acc: i16 = 0;
  let mut %i:   i32 = 0;
  let mut %wide: i32 = 0;
  let mut %r:   i32 = 0;
^loop:
  %acc  = call @saturating_add(%acc, %xs[%i]);
  %acc  = call @wrapping_mul(%acc, 3 as i16);
  %i    = %i + 1;
  br %i < 4, ^loop, ^done;
^done:
  %wide = %acc as i32;
  %r    = call @rem_euclid(%wide, %m);
  ret %r;
}
```

### Example — bit-manipulation family

```text
intrinsic @bitreverse(%x: i32) : i32;
intrinsic @rotl(%x: i32, %n: i32) : i32;
intrinsic @is_pow2(%x: i32) : i1;

fun @demo(%x: i32) : i32 {
  let mut %r: i32 = 0;
  let mut %p: i1 = 0;
  let %four: i32 = 4;
^entry:
  %r = call @rotl(%x, %four);
  %r = call @bitreverse(%r);
  %p = call @is_pow2(%r);
  br %p, ^pow2, ^other;
^pow2:
  ret %r;
^other:
  ret %r - %r;     // returns 0
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
| `@abs_diff` | `(iN, iN) → iN` | `\|a − b\| > INT_MAX_N` | `[0, INT_MAX_N]` |
| `@signum` | `(iN) → iN` | — | `{−1, 0, +1}` |
| `@clamp` | `(iN, iN, iN) → iN` | `lo > hi` (signed) | `[lo, hi]` |
| `@midpoint` | `(iN, iN) → iN` | — | `[INT_MIN_N, INT_MAX_N]` |
| `@parity` | `(iN) → i1` | — | `{0, 1}` |
| `@bswap` | `(iN) → iN`, `N % 8 == 0` | — (declaration rejected if `N` is not a multiple of 8) | full `iN` |
| `@bitreverse` | `(iN) → iN` | — | full `iN` |
| `@rotl` | `(iN, iN) → iN` | `n < 0` or `n >= N` | full `iN` |
| `@rotr` | `(iN, iN) → iN` | `n < 0` or `n >= N` | full `iN` |
| `@is_pow2` | `(iN) → i1` | — | `{0, 1}` |
| `@ilog2` | `(iN) → iN` | `x <= 0` (signed) | `[0, N − 2]` |
| `@wrapping_add` | `(iN, iN) → iN` | — | full `iN` |
| `@wrapping_sub` | `(iN, iN) → iN` | — | full `iN` |
| `@wrapping_mul` | `(iN, iN) → iN` | — | full `iN` |
| `@wrapping_neg` | `(iN) → iN` | — | full `iN` |
| `@wrapping_shl` | `(iN, iN) → iN` | `n < 0` or `n >= N` | full `iN` |
| `@wrapping_shr` | `(iN, iN) → iN` | `n < 0` or `n >= N` | full `iN` |
| `@saturating_add` | `(iN, iN) → iN` | — | `[INT_MIN_N, INT_MAX_N]` |
| `@saturating_sub` | `(iN, iN) → iN` | — | `[INT_MIN_N, INT_MAX_N]` |
| `@saturating_mul` | `(iN, iN) → iN` | — | `[INT_MIN_N, INT_MAX_N]` |
| `@saturating_neg` | `(iN) → iN` | — | `[INT_MIN_N, INT_MAX_N]` |
| `@div_euclid` | `(iN, iN) → iN` | `b == 0` or `(a == INT_MIN_N ∧ b == -1)` | `[INT_MIN_N, INT_MAX_N]` |
| `@rem_euclid` | `(iN, iN) → iN` | `b == 0` or `(a == INT_MIN_N ∧ b == -1)` | `[0, \|b\| − 1]` |

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

## Priority tiers and rejection policy

The six intrinsics above are only the seed of the design space. The
target surface spans the full C `<math.h>` / WASM numeric / Rust
`iN`/`fN` API. We deliberately do **not** intend to implement all of it:
many functions (transcendentals, recursive number theory) have no
efficient SMT-BV / QF_FP encoding, and shipping them anyway would let
users write programs that hang the solver. Each candidate intrinsic is
therefore classified by **how cleanly the solver and the C backend can
handle it** — those two backends drive prioritization; WASM is the
second-to-last priority; anything the solver cannot handle precisely is
the last priority.

### Rejection layers

Each intrinsic carries per-backend support flags. Calling an
unsupported intrinsic produces an error at the earliest layer that can
detect it:

| Layer | Rejection means | Used for |
|---|---|---|
| **Frontend (semantic checker)** | `intrinsic @x` declaration is refused. Program will not parse. | Truly nonsensical or stateful intrinsics: `@rand`, `@time`, anything impure, anything that produces non-finite FP. |
| **Solver (`symirsolve`)** | Declaration and program are accepted; reaching a `call @x` on a *symbolic* path makes that path infeasible (the same effect as UB pruning). Concrete-only paths still solve. | Transcendentals, recursive number theory, anything without a precise SMT encoding. |
| **WASM backend** | Compile-time error from `symirc --target wasm`: "`@x` not lowerable to WASM target". | Intrinsics with no native WASM op whose polyfill would diverge from the C target (e.g. `wasi-libc` vs `glibc` libm last-ULP drift). The C target may still accept them. |
| **Interpreter** | Runtime error (distinct from UB: "intrinsic not implemented in this build"). | Reserved. The interpreter is the reference oracle; aim to keep this empty. |

**Consistency rule.** The interpreter must agree with every other
backend on every value. When a libm-based intrinsic is supported by
both the C backend and the interpreter, both must link against the
same libm so cross-validation is byte-equal by construction.

### Tier summary

| Tier | Solver | C | WASM | Interp | Plan |
|---|---|---|---|---|---|
| **P0** | ★ / ◐ | ★ | ★ / ◐ | ★ | v0.2.2: ship in four batches (below) |
| **P1** | ★ / ◐ | ★ | ◐ / ◑ (composed lowerings) | ★ | Planned for a later version |
| **P2** | ◑ (bounded encoding, may time out) | ★ | ◑ | ★ | Planned behind a feature flag |
| **P3** | rejects path | ★ via libm | rejected (or libm with ULP drift) | ★ via libm | Planned |
| **P4** | — | — | — | — | Rejected at frontend, permanently |

Difficulty legend: ★ trivial · ◐ easy · ◑ medium · ◯ hard but feasible.

### P0 — solver-★/◐, C-★, WASM-★/◐

P0 is split into three groups by domain. The integer extras and
bit-manipulation groups are **shipped** (§12.3 and §12.4 above). The
integer overflow-aware family and the floating-point basic IEEE family
are **planned** for later v0.2.2 work — the FP family additionally
needs the §12 type-restriction sentence ("`iN` denotes any concrete
integer type") to be relaxed before it can land, and that change is
held back by the §13 non-goal bullet listed below.

**v0.2.2 extra batch A - Integer extras** (shipped — §12.3): `@abs_diff`, `@signum`, `@clamp`,
`@midpoint`.

**v0.2.2 extra batch B - Bit-manipulation** (shipped — §12.4): `@parity`, `@bswap`,
`@bitreverse`, `@rotl`, `@rotr`, `@is_pow2`, `@ilog2`.

**v0.2.2 extra batch C - Integer overflow family** (scalar-result subset shipped — §12.5):
`@wrapping_{add,sub,mul,neg,shl,shr}`,
`@saturating_{add,sub,mul,neg}`,
`@div_euclid`, `@rem_euclid`.
The tuple-returning members (`@checked_*`, `@overflowing_*`) and the
cross-width `@widening_mul` (`iN×iN → i2N`) are slated for a follow-up
batch that introduces the multi-value return ABI.

**v0.2.2 extra batch D - Floating-point basic IEEE family** (planned —
gated on the §12 type-restriction sentence; held shut by §13 until this
batch lands). All entries map to QF_FP ops and direct WASM `fN.*`
opcodes:
`@fabs`, `@fneg`, `@copysign`, `@fmin`, `@fmax`, `@sqrt`, `@fma`,
`@floor`, `@ceil`, `@trunc`, `@rint` (ties-to-even),
`@signbit`, `@is_normal`, `@is_subnormal`,
`@to_bits`, `@from_bits`,
`@ldexp`, `@scalbn`, `@ilogb`, `@logb`,
`@fract`, `@recip`, `@to_degrees`, `@to_radians`.

### P1 — solver-easy, WASM-tricky (planned)

Solver and C lowerings remain trivial; WASM has no direct op and needs
a small composition. Ship after P0 completes.

`@ffs` (`ctz+1` with 0→0), `@next_pow2`, `@round` (away-from-zero;
distinct from `@rint` ties-to-even), `@fmod` (truncated remainder —
**not** the same as `@remainder`), `@remainder` (IEEE `fp.rem`),
`@fdim`, `@modf`, `@frexp`, `@nextafter`, `@fpclassify`, `@total_cmp`,
saturating fp→int conversions (Rust's default `as`).

Vector reductions (`@reduce_add`, `@reduce_max`, …) also land here in
spirit but are bundled with the v0.2.3 SIMD work — see spec §13.

### P2 — solver-feasible-but-expensive (planned, behind a flag)

Encodable in SMT but at quadratic-or-worse cost. Gate behind
`--enable-experimental-intrinsics` and enforce a per-call solver
timeout with a clear "intrinsic @X is too expensive on this path"
diagnostic.

`@isqrt` (bit-by-bit synthesis), `@pow(b, e)` with symbolic `e`
(bounded unroll up to `--max-pow-iters`), `@ilog10` (decimal-power ITE
chain), `@hypot` with both args symbolic (nonlinear FP).

### P3 — solver rejects path, libm on host targets (planned)

The C backend and the interpreter link the host libm; calling these
is fine on concrete inputs and on symbolic paths the user does not
intend to solve. Reaching one on a path passed to `symirsolve` prunes
the path, identically to UB.

The WASM backend rejects these at compile time. `wasi-libc`'s libm
diverges from `glibc` at the last ULP for transcendentals, which would
silently break xval; the C-target-only contract is simpler than
tolerating per-target ULP drift in test harnesses.

- **Transcendentals:** `@exp`, `@exp2`, `@expm1`, `@log`, `@log2`,
  `@log10`, `@log1p`, `@pow`/`@powf` with symbolic exponent, `@cbrt`,
  `@sin`, `@cos`, `@tan`, `@asin`, `@acos`, `@atan`, `@atan2`,
  `@sin_cos`, `@sinh`, `@cosh`, `@tanh`, `@asinh`, `@acosh`, `@atanh`,
  `@erf`, `@erfc`, `@tgamma`, `@lgamma`. Bessel (`@j0`/`@y0`/`@jn`/
  `@yn`) only if there is concrete demand.
- **Number theory:** `@gcd`, `@lcm`, `@ilog(base)` with symbolic base.

### P4 — frontend rejects (planned, permanent)

Declarations of these are refused outright — there is no point letting
a user write a program SymIR cannot reason about.

- **Stateful / impure:** `@rand`, `@srand`, `@time`, `@clock`,
  `@getpid`, environment access. Would break determinism: re-execution
  must give the same result.
- **I/O:** `@printf`, `@scanf`, `@fopen`, file/network handles.
- **Non-finite FP producers:** `@nan`, `@inf`. The finite-only FP
  domain (spec §2.9) is a hard invariant.
- **Byte-level memory intrinsics:** `@memcpy`, `@memset` — already
  deferred in spec §13. Require byte-level array reasoning the solver
  backend does not yet support. May be re-promoted to a higher tier if
  the solver gains byte-addressable memory.
