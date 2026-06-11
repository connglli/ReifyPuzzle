# RefractIR Floating-Point

This document consolidates every floating-point commitment RefractIR makes,
across the spec, the interpreter (`symiri`), the C/WASM backends
(`symirc`), and the solver (`symirsolve`). It is a companion to the
formal spec — every section names the spec reference it derives from —
and to its siblings [`undefined.md`](./undefined.md) (UB rules,
per-tool) and [`intrinsics.md`](./intrinsics.md) (intrinsic taxonomy).

RefractIR's FP design has one overriding principle:

> **Every FP operation produces the same bit pattern on every backend.**

That is the *only* property worth defending. Everything below — the
finite-only domain, RNE everywhere, `fmod` semantics, the canonical
serialization invariant, the libm-sameness rule for future intrinsics —
exists to keep that property mechanical and checkable, not aspirational.

---

## 1. Value model (spec §2.9)

- **Types.** `f32` (IEEE 754 binary32) and `f64` (IEEE 754 binary64).
  No `f16`, `f128`, `bfloat16`, x87 80-bit extended, or decimal floats.
- **Domain — finite only.** The only valid RefractIR FP values are
  *finite* IEEE 754 values. `±∞` and NaN are **not** RefractIR values; any
  op whose IEEE result would be `±∞` or NaN is **UB** (§7.4 rules 6–7).
  Programs that depend on infinity or NaN propagation are outside the
  language.
- **Signed zero.** `+0.0` and `-0.0` are distinct bit patterns,
  both valid. They compare equal (`0.0 == -0.0` is `true`) but are
  distinguishable by `@signbit` (planned, batch D) and by `to_bits`.
  The canonical serializer preserves the sign bit across every text
  boundary.
- **Subnormals.** Subnormal (denormal) values are first-class finite
  values. RefractIR does **not** flush-to-zero, and no backend may enable
  FTZ/DAZ. `parseFloatLiteral` accepts subnormals; only true overflow
  to `±HUGE_VAL` is rejected.
- **Rounding.** All ops use **round-to-nearest, ties-to-even** (IEEE
  `roundNearestTiesToEven` / SMT `RNE`). There is no alternate rounding
  mode, no `fenv` access, and no rounding mode parameter on any
  intrinsic.

SMT sort mapping:

| RefractIR type | SMT sort               |
|------------|------------------------|
| `f32`      | `(_ FloatingPoint 8 24)`  |
| `f64`      | `(_ FloatingPoint 11 53)` |

---

## 2. Operations and rounding

All scalar operators are typed homogeneously: every atom in a `+`/`-`
chain shares the exact same FP type; `*`, `/`, `%` and atom-level coefs
likewise. Mixed-width or int↔float arithmetic requires an explicit `as`
cast (§6.7).

| Op  | Semantics                                  | Rounding | UB rule  |
|-----|--------------------------------------------|----------|----------|
| `+` | IEEE add                                   | RNE      | §7.4-6,7 |
| `-` | IEEE sub                                   | RNE      | §7.4-6,7 |
| `*` | IEEE mul                                   | RNE      | §7.4-6,7 |
| `/` | IEEE div                                   | RNE      | §7.4-6,7 |
| `%` | C `fmod` (truncated-quotient remainder)    | RNE      | §7.4-6,7 |

Each op's result must be **finite** (not `±∞`, not NaN); otherwise the
path is UB. This is the cross-backend contract: each backend asserts
finiteness after every FP arithmetic op.

There are no other built-in FP operators in the v0.2.2 language.
Everything else (`sqrt`, `fabs`, `floor`, `ceil`, `trunc`, `rint`,
`fma`, …) is reserved for the *intrinsic* layer — see [`intrinsics.md`
batch D](./intrinsics.md#p0--solver-doc-c--wasm-doc).

### 2.1 Comparison

`cmp <relop>` lifts to FP transparently. Lane-wise on vectors, scalar
otherwise. Result type is `i1` (scalar) or `<N> i1` (mask).

| relop      | Meaning                                                |
|------------|--------------------------------------------------------|
| `==`, `!=` | IEEE numeric equality. `+0.0 == -0.0` is `true`.       |
| `<`, `<=`  | IEEE ordered less-than / less-equal.                   |
| `>`, `>=`  | IEEE ordered greater-than / greater-equal.             |

Because NaN is UB, the "unordered" cases of the IEEE relops never
arise: by the time `cmp` runs, both operands are finite, so every relop
is total.

### 2.2 `select` and FP

`select cond, a, b` evaluates only the selected arm (lazy). The unused
arm's UB does **not** fire (§7.2). This means `select (y != 0.0), x/y, 0.0`
is a legal idiom to dodge divide-by-zero UB on the `y == 0` path.

---

## 3. The `%` operator: `fmod`, not `remainder`

This is the most-likely-to-trip-up corner of the language.

RefractIR's `%` is **C `fmod`** (truncated quotient), **not** IEEE 754
`remainder` (`fp.rem`):

> `x % y = x - trunc(x / y) * y`

where `trunc` rounds toward zero. The sign of the result follows the
sign of `x`, matching integer `%` (§2.5).

`fmod` differs from `remainder` in two visible ways:
- **Sign rule.** `remainder` returns the result with the smallest
  absolute value, possibly negative when `x > 0`; `fmod` preserves
  `sign(x)`.
- **Half-quotient ties.** When `x/y` is exactly a half-integer,
  `remainder` rounds to even; `fmod` rounds toward zero.

If a future intrinsic needs IEEE `remainder` semantics, that lands as
`@remainder` (tier P1 in `intrinsics.md`) — explicitly distinct from
`@fmod` (also P1, redundant with `%` but kept for clarity at the source
level).

UB cases for `%`:
- `x % 0.0` (any signed zero) — IEEE result is NaN — UB (§7.4-7).
- `x` finite, `y` finite, non-zero, but result overflows — cannot
  happen mathematically for `fmod` (the true remainder has magnitude
  `< |y|`); only spurious from the SMT/WASM lowering, see §11.

---

## 4. Casts (`as`)

All four directions are well-typed under §6.4. Each cast is a
**single-rounding** op under RNE, except for the f32→f64 widening
which is always exact.

| Cast            | Semantics                                                                         | UB                                |
|-----------------|-----------------------------------------------------------------------------------|-----------------------------------|
| `iN as fM`      | Convert signed integer to FP under RNE.                                           | If result overflows (e.g. `i64` → `f32` near `2^128`). |
| `fN as iM`      | Truncate toward zero, then check fit.                                             | Out-of-range after truncation (§7.4-8).               |
| `f32 as f64`    | Exact widening (every f32 is an f64).                                             | Never.                            |
| `f64 as f32`    | Round-to-f32 under RNE.                                                           | If the rounded result is `±∞`.    |

Vector casts apply per lane.

There are no pointer↔float casts (forbidden by §13 non-goals).

---

## 5. UB rules (recap)

The three FP UB rules from §7.4 also appear in
[`undefined.md` §Scalar arithmetic](./undefined.md#scalar-arithmetic-71-74),
which is the per-tool enforcement reference. Summary:

- **Rule 6 — FP overflow.** Any `+`, `-`, `*`, `/` whose RNE result
  would be `±∞` is UB.
- **Rule 7 — FP invalid (NaN).** Any op whose result would be NaN is UB.
  Covers `0/0` and `x % 0`.
- **Rule 8 — Float-to-int out-of-range.** `fN as iM` is UB if the
  truncated mathematical value is outside `iM`'s representable range.

Per-lane analogues apply to vectors (rule 21).

---

## 6. Vector FP (spec §2.11, §6.9)

- Vectors `<N> f32` and `<N> f64` are first-class value types. `N ≥ 2`.
- Lane-wise arithmetic: every scalar op lifts to the corresponding
  lane-wise op. Rounding mode, UB rules, and `cmp` semantics are
  identical to scalar — applied per lane.
- `cmp <relop> v, w` on FP vectors yields `<N> i1` (a per-lane mask).
- No FP-vector intrinsics ship in v0.2.2. Horizontal reductions
  (`@reduce_add`, `@reduce_max`, …) are tier P1 (vector SIMD work,
  §13).

Vectors are not addressable (no `ptr <N> T`, no `addr` on a vector
local) — see §13 non-goals.

---

## 7. Literals and inference

- **Float literal token.** `1.5`, `-0.2`, `1e-5`, `3.14E+2`.
  Decimal-only — **no hex-float literals** at the source level.
- **No `inf`, `nan`, `INFINITY`, `NAN` literal forms.** Programs cannot
  construct non-finite values; that follows directly from the
  finite-only domain.
- **Default inference.** A bare float literal is `f32` if the
  surrounding context does not force `f64`. Coexisting with the integer
  default of `i32`, this matches the "narrow if possible" tradition
  C / Rust users expect.
- **Bit-exact parsing.** Every float literal is parsed by
  `refractir::parseFloatLiteral` (which wraps `std::strtod` — never
  `std::stod`, see §9 below).
- **`undef` of FP type** is allowed at the type level (any leaf may be
  `undef`); reading it is UB (§7.1 rule 3).

---

## 8. Coexistence with integer `%` semantics

The `%` operator on integers is also truncated-quotient (`bvsrem`,
C-like), and `fmod` on floats was chosen to match that. The
language-level mental model is:

> `x % y` always means "the remainder when `x / y` is truncated toward zero."

uniformly for both integers and floats. This avoids an asymmetry that
trips up users who write `% 0.5` for fractional-part tricks (where
`remainder` would give a negative remainder when `x > 0`).

---

## 9. Canonical serialization invariant (CLAUDE.md, MANDATORY)

RefractIR carries `f32`/`f64` values **bit-exactly** across every text
boundary: `.sir` source, descriptor JSON, SOLVED/PARAMS/RETURN headers,
model-dump files, and CLI positional args. Two canonical entry points
own this:

- **`refractir::formatDouble(double)`** (`include/ast/ast.hpp`) — shortest
  decimal string that round-trips via
  `std::to_chars(…, std::chars_format::shortest)`, with `.0` appended
  if neither `.` nor exponent appears (so an integer-valued literal
  still tokenises as float). Preserves signed zero.
- **`refractir::parseFloatLiteral(std::string)`** — wraps `std::strtod`.
  Accepts subnormals; only true overflow to `±HUGE_VAL` raises.
  **Never use `std::stod`** — libstdc++ throws `out_of_range` on any
  `ERANGE` including valid subnormals.

Intentional, documented divergences:

| Site                       | Why                                            |
|----------------------------|------------------------------------------------|
| `src/backend/c_backend.cpp`   | Emits **C grammar** floats (`f`/`F` suffix). Its own bit-exact formatter; comments point back to `refractir::formatDouble`. |
| `src/backend/wasm_backend.cpp`| Emits **WAT grammar** floats (`±inf`/`nan` syntax, `f32.const`/`f64.const`). Its own bit-exact formatter. |

If you are about to write `std::stod`, `std::to_string(double)`,
`std::ostringstream` with `precision(17)`, `printf("%.17g", …)`, or
`printf("%f", …)` in RefractIR code — **stop and use the canonical pair**.

The interpreter emits its `Result:` line via `printf("%a", …)` (hex
float). Hex-float form is parseable by `strtod` and bit-exact by
construction — that is what the xval harness compares against the
C-side `printf("Result: %a\n", …)`. Hex float as a *source-level*
literal is **not** supported; this is purely a serialization channel
for results.

---

## 10. SMT encoding (spec §9, §2.9)

All FP reasoning happens in **QF_FP** (CVC5, Z3, Bitwuzla all support
this; RefractIR does not require any solver-specific extension).

- Sorts: `(_ FloatingPoint 8 24)` and `(_ FloatingPoint 11 53)`.
- A single `roundNearestTiesToEven` rounding-mode constant is created
  once per `Solver` and threaded through every FP op.
- After every FP `+`, `-`, `*`, `/`, the encoder conjoins
  `(not (fp.isInfinite t)) AND (not (fp.isNaN t))` to `PC`. The
  solver's `assertFPFinite` helper centralizes this; see
  `solver.cpp:1698`.
- Symbol values (`sym %?x: value f32`) are encoded as fresh FP
  constants of the corresponding sort. No `sym` of vector-FP type is
  rejected — vector FP symbols are per-lane independent.
- `%` (fmod) encodes as
  `fp.sub(x, fp.mul(fp.roundToIntegral[RTZ](fp.div[RNE](x, y)), y))`
  — see §11 for an important caveat about this encoding.
- Casts encode through `fp.to_fp` / `fp.to_sbv` with RNE for the
  conversion step and an explicit BV-range check for `fN as iM`.

---

## 11. Lowering principles and the cross-backend contract

The bit-exactness goal partitions into three responsibilities.

### 11.1 Interpreter (`symiri`)

- Sets `std::fesetround(FE_TONEAREST)` at startup, and on x86 also
  clears MXCSR FTZ and DAZ so subnormals are not flushed in case a
  parent process or upstream library left them set.
- Stores f32 values as `double` in `floatVal`; after every f32 op,
  narrows via `static_cast<double>(static_cast<float>(v))` and runs
  `std::isinf`/`std::isnan` finiteness checks (`checkFPResult`,
  `interpreter.cpp:16`). This narrow-then-check pattern is bit-exact
  with single-rounded f32 arithmetic by an "innocuous double
  rounding" theorem: if the intermediate precision satisfies `q ≥
  2p + 1` where `p` is the target precision, computing in `q` then
  rounding to `p` matches single-rounded precision-`p` arithmetic
  for every RNE-rounded binary op among `+ - × ÷ √`. RefractIR uses
  `p = 24` (f32), `q = 53` (f64), so `53 ≥ 49` ✓
  (Boldo & Melquiond, *When Double Rounding is Odd*, 2005).
- Parameter binding rounds f32 args to f32 precision before the body
  runs. Stored f32 locals truncate the held `double` to 4-byte storage
  so re-reads agree with the C backend (`interpreter.cpp:825`,
  `998`, `1222`).
- `%` calls `std::fmod` / `std::fmodf` from libm, with an explicit
  §2.9 intermediate-`x/y` finiteness check before the fmod call so
  the operand-precision overflow rule (§7.4 rule 6 applied to the
  inner `fp.div` of the encoding) is enforced consistently with the
  WASM and solver paths.
- `as` to integer pre-bounds-checks against `[-2^(bits-1), 2^(bits-1))`
  before `static_cast<int64_t>`.

### 11.2 C backend (`symirc --target c`)

- Emits `float` / `double` for `f32` / `f64`.
- Emits float literals via the backend's own bit-exact formatter
  (intentional divergence from `formatDouble` — different grammar).
- Emits `fmodf` / `fmod` for `%`, wrapped in a GCC statement
  expression that pre-evaluates `x/y` and traps on a non-finite
  intermediate, enforcing §2.9 alongside the interpreter and WASM
  paths.
- Forces lane-unroll for float vector `%` so each lane goes through
  the lane-wise fmod helper (GCC vector extensions don't define `%`
  for floats, so the inline path would otherwise emit invalid C).
- Emits explicit `isinf` / `isnan` checks after every FP arithmetic op
  (UBSan's `-fsanitize=float-divide-by-zero` does not catch
  finite-overflow-to-∞ by itself).
- Pins FP behaviour **at the source level**, not via harness compile
  flags. Every emitted compilation unit (via `common.h`) carries
  three guards:
  ```c
  #if !defined(__STDC_IEC_559__) || __STDC_IEC_559__ != 1
  # error "RefractIR-lowered C requires an IEC 60559 / IEEE 754 conforming implementation"
  #endif
  #if !defined(FLT_EVAL_METHOD) || FLT_EVAL_METHOD != 0
  # error "RefractIR-lowered C requires an implementation with FLT_EVAL_METHOD == 0"
  #endif
  #pragma STDC FP_CONTRACT OFF
  ```
  The C standard mandates the `#pragma` override any `-ffp-contract`
  flag the caller passes, and `FLT_EVAL_METHOD == 0` rules out x87 /
  `long double` excess-precision evaluation. The xval and reify-diff
  harnesses therefore do not need to pass any extra FP flags.
- Required compile flags for cross-validation: `-fsanitize=undefined
  -fno-sanitize-recover=all -lm`. `-Ofast` and `-ffast-math` are
  forbidden (they bypass the source-level pragma).

### 11.3 WASM backend (`symirc --target wasm`)

- Emits `f32.const` / `f64.const` via its own bit-exact formatter.
- Emits `f32.{add,sub,mul,div}` / `f64.{add,sub,mul,div}` directly.
- `%` is composed inline as the §2.9 encoding `x - fN.trunc(x / y) * y`
  (no native `fN.rem` instruction in WASM MVP). The §2.9 finiteness
  rule on the intermediate `x/y` falls out naturally because a
  non-finite intermediate propagates to the final result, which the
  post-op finiteness check would catch — **except** the WASM backend
  does not yet emit a post-op finiteness check for `%` (tracked
  follow-up; the four `fp_rem_intermediate_overflow_*.sir` tests
  carry `// SKIP: WASM` until this lands).
- f32↔f64 conversion uses `f64.promote_f32` / `f32.demote_f64`.

### 11.4 Solver (`symirsolve`)

- Already described in §10.

### 11.5 Reify-diff cross-validation

The reify-diff harness solves a path, reifies the model into a concrete
`.sir`, runs the interpreter, compiles the C output, runs both, and
**diffs the `Result:` line byte-equally**. The interpreter prints via
`printf("%a", …)`; the C harness's main also prints via `%a`. Hex-float
output makes every bit observable.

The xval test harness compiles with **`gcc … -fsanitize=undefined
-fno-sanitize-recover=all -lm`** (no opt level explicitly set, so
default `-O0`). The harness deliberately passes no `-ffp-contract`
or `-fexcess-precision` flag — those concerns are pinned at the
source level by the C backend's emitted `#pragma STDC FP_CONTRACT
OFF` and `FLT_EVAL_METHOD == 0` `#error` guards (§11.2).

### 11.6 Libm-sameness rule (forward-looking)

When a future intrinsic is **libm-backed** on the C and interpreter
targets (planned for tier P3 — transcendentals like `@exp`, `@log`,
`@sin`), both targets must link against the *same* libm so that the
last-ULP behaviour is byte-equal by construction. The WASM target
**rejects** any such intrinsic at compile time ("no native WASM op
whose polyfill would diverge from the C target"). The solver rejects
the path. See [`intrinsics.md` §Tier summary](./intrinsics.md).

This rule does not apply to batch D — every batch D intrinsic is
either correctly-rounded by IEEE 754 (so libm divergence cannot occur)
or pure bit manipulation. That is the design criterion that makes
batch D shippable in the first place.

---

## 12. What is intentionally excluded

These are not gaps to fix; they are explicit non-goals that simplify
the cross-backend contract.

| Excluded                                       | Why                                                                                                                                |
|------------------------------------------------|------------------------------------------------------------------------------------------------------------------------------------|
| `±∞`, NaN as values                            | Single-source-of-truth domain. No quiet/signaling NaN distinction, no NaN payload, no NaN-vs-NaN comparison weirdness.             |
| Alternate rounding modes (`RTZ`, `RU`, `RD`, `RMM`) | RNE is universal across SSE2 / AArch64 / WASM / SMT. Other modes would require fenv plumbing or per-op rounding parameters.       |
| `fenv` access (`feenableexcept`, `fegetround`, FP exceptions) | Process-level mutable state; impossible to reproduce in WASM, and breaks SMT semantics.                                            |
| Decimal floats, `f16`, `f128`, bfloat16, x87 80-bit | None are universally available across all backends.                                                                                |
| Hex-float source literals (`0x1.8p+3`)         | Decimal literals plus the canonical bit-exact serializer already round-trip every value. Hex floats are a serialization-only form. |
| `signaling NaN`, `quiet NaN`, NaN payloads     | Outside the finite-only domain by construction.                                                                                    |
| Subnormal flush-to-zero (`FTZ`/`DAZ`)          | Subnormals are first-class. Backends must not enable FTZ/DAZ.                                                                      |
| FMA contraction (`a*b+c → fma(a,b,c)`)         | A `*` followed by `+` is two separate ops with two separate UB-finiteness checks. Implicit contraction would silently change the rounding behaviour. The C backend emits `#pragma STDC FP_CONTRACT OFF` to forbid it at the source level (§11.2). |
| Pointer ↔ float casts                          | Forbidden in v0.2.2 (spec §13 non-goals).                                                                                          |

---

## 13. Forward look — batch D and beyond

Batch D (the FP basic IEEE family — `@fabs`, `@fneg`, `@copysign`,
`@fmin`, `@fmax`, `@sqrt`, `@fma`, `@floor`/`@ceil`/`@trunc`/`@rint`,
`@signbit`, `@is_normal`, `@is_subnormal`, `@to_bits`/`@from_bits`,
`@ldexp`/`@scalbn`/`@ilogb`/`@logb`, `@fract`, `@recip`,
`@to_degrees`/`@to_radians`) is precisely the subset of FP intrinsics
where bit-exactness across backends is guaranteed by either pure
bit-manipulation or IEEE-required correct rounding. Three care points:

- `@fma` must lower to `fma()`/`fmaf()` (libm-backed), never `x*y+z`.
- `@rint` must use a fenv-independent lowering (`__builtin_roundeven*`
  or SSE4.1 `roundsd` with the RNE immediate baked in).
- `@to_degrees`/`@to_radians` must pin the conversion constant to a
  documented bit pattern in the spec.

Tiers P1 (composed-WASM lowerings) and beyond — including IEEE
`@remainder`, `@fmod` as an intrinsic, transcendentals, vector
reductions — defer to later versions. See [`intrinsics.md` Tier
summary](./intrinsics.md).

The libm-sameness rule (§11.6) applies only to tier P3 and beyond.
Batch D is exempt by construction.
