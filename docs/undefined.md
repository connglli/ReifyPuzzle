# RefractIR Strict Undefined Behavior (UB)

This document is a per-rule companion to the formal spec (§7 of `SPEC_v0.2.2.md`). Each section names the rule, the spec reference, and how each of `symiri` (interpreter), `symirc` (compiler), and `symirsolve` (solver) enforces it.

RefractIR uses **strict UB**: if any operation on the executed path triggers UB, the entire path is **infeasible**.

- `symiri` aborts on UB (immediate termination, non-zero exit).
- `symirc`-emitted C runs under UBSan with `-fno-sanitize-recover=all`, so UB traps the executable.
- `symirsolve` adds the UB-precluding constraint to `PC`; satisfiable models avoid the UB.

The rule numbers below match the formal spec's UB rule numbers; rules unique to v0.2.1 are marked **[v0.2.1]**.

---

## Scalar arithmetic (§7.1, §7.4)

### Rule 1 — Integer division/modulo by zero
`a / b` or `a % b` with `b == 0` is UB.

- **symiri:** checks divisor before the op.
- **symirc:** UBSan `-fsanitize=integer-divide-by-zero`.
- **symirsolve:** `(distinct b 0)` per division site.

### Rule 2 — Out-of-bounds array access
`a[i]` where `a : [N] T` and `i < 0 ∨ i >= N` is UB.

- **symiri:** bounds-checks every lvalue index.
- **symirc:** UBSan `-fsanitize=array-bounds` (for fixed-size arrays).
- **symirsolve:** `(bvult i N)` (unsigned-less-than handles negative as large positive).

### Rule 3 — Reading `undef`
Reading any leaf whose stored value is `undef` is UB. This subsumes uses of uninitialised locals, uninitialised pointer values, and uninitialised vector lanes.

- **symiri:** per-leaf `undef` tracking; throws on read.
- **symirc:** emitted code initialises every scalar leaf at declaration; `undef` is lowered to a literal `0` plus a path-pruning `assume(false)` token, so the compiled program either zero-reads or traps under UBSan.
- **symirsolve:** every symbolic value carries an `is_defined` flag; reads conjoin it to `PC`.

Also enforced statically by `DefiniteInitAnalysis` (warning-level, conservative).

### Rule 4 — Signed integer overflow
`+`, `-`, `*`, `<<` whose result is outside the signed range of the target width is UB. Also `INT_MIN / -1`. RefractIR treats `<<` as signed arithmetic (not BV wrap), so `x << n` is UB if `x * 2^n` doesn't fit OR if `x < 0`.

- **symiri:** checks via per-op signed-overflow predicate.
- **symirc:** UBSan `-fsanitize=signed-integer-overflow,shift`.
- **symirsolve:** `bvsaddo/bvssubo/bvsmulo` overflow predicates; for `<<`, an explicit reconstruct-and-compare.

### Rule 5 — Overshift
`x << n`, `x >> n`, `x >>> n` with `n < 0` or `n >= width(x)` is UB. This is about the *amount*; the shifted result is covered by rule 4.

- **symiri:** range-checks the shift amount.
- **symirc:** UBSan `-fsanitize=shift`.
- **symirsolve:** `(bvult n width)`.

### Rule 6 — FP overflow (±∞ result)
Any `+`, `-`, `*`, `/` whose RNE-rounded result would be ±∞ is UB. Covers finite-operand overflow and `x / ±0.0` for non-zero `x`.

- **symiri:** `std::isinf` after every FP op.
- **symirc:** UBSan `-fsanitize=float-divide-by-zero` plus an explicit `isinf` check on every FP arithmetic result (the C backend emits the check; UBSan does not catch overflow-to-inf by itself).
- **symirsolve:** `(not (fp.isInfinite result))` on each FP op.

### Rule 7 — FP invalid (NaN result)
Any FP op producing NaN is UB. Covers `±0.0 / ±0.0` and `x % ±0.0`.

- **symiri:** `std::isnan` after every FP op.
- **symirc:** emitted `isnan` check after each FP op.
- **symirsolve:** `(not (fp.isNaN result))`.

### Rule 8 — Float-to-integer out-of-range
`fN as iM` is UB if the truncated value (toward 0) is outside the representable range of `iM`.

- **symiri:** pre-cast bounds check.
- **symirc:** UBSan `-fsanitize=float-cast-overflow`.
- **symirsolve:** `(bvsle min_iM rounded)` and `(bvsle rounded max_iM)`.

---

## Pointer UB (§7.5)

### Rule 9 — Null pointer dereference
`load %p` or `store %p, v` with `%p == null` is UB.

- **symiri:** checks pointer ≠ `nullptr` before any deref.
- **symirc:** the emitted load/store traps under UBSan's null sanitiser.
- **symirsolve:** `(distinct %p (_ bv0 64))` on every deref.

### Rule 10 — Out-of-bounds pointer arithmetic [revised in v0.2.1]
Every pointer carries a **provenance object** (rule 15). Pointer arithmetic `%p ± n` is UB if the resulting address falls outside `[base_provenance, base_provenance + size_provenance]`. The one-past-the-end address is valid for arithmetic and equality, UB to `load`/`store` (rule 11).

- **symiri:** every `ptr` runtime value tags its `ObjectInfo` (base address + size in bytes). Arithmetic updates the offset; if `new_off ∉ [0, size]` the runtime aborts.
- **symirc:** in v0.2.1, `addr` of an aggregate / `ptrindex` / `ptrfield` lowers to GCC `__builtin_object_size`-instrumented arithmetic; UBSan `-fsanitize=pointer-overflow` catches the wrap; an explicit emitted range check covers the in-bounds path.
- **symirsolve:** path condition gets `(bvule new_off size)` (one-past-end allowed). Each `addr` operand records its provenance's base and size for downstream propagation.

### Rule 11 — Out-of-bounds load/store
`load %p` / `store %p, v` is UB if `%p` lies outside `[base, base + size)` (one-past-the-end is included as "outside" for the purpose of deref).

- **symiri:** combines rule 10's `ObjectInfo` with a strict `< size` check at deref.
- **symirc:** UBSan + the emitted bound check.
- **symirsolve:** `(bvult offset size)` on every `load`/`store`.

### Rule 12 — Cross-object pointer arithmetic
Forming a pointer by arithmetic that crosses from one local's storage into another's is UB.

- **symiri:** rule 10's `ObjectInfo` won't cross objects — the offset would exit the known allocation, triggering UB before any cross-object value is produced.
- **symirc:** UBSan catches the boundary cross via `-fsanitize=pointer-overflow`.
- **symirsolve:** non-overlap axioms (§9.4.2) make cross-object arithmetic infeasible by construction.

### Rule 13 — Uninitialised pointer dereference
`load %p` or `store %p, v` where `%p == undef` is UB (a consequence of rule 3 specialised to pointer values, kept as a separate rule for clarity).

- **symiri:** per-leaf `undef` flag, checked before deref.
- **symirc:** emitted code initialises pointer leaves to `null`, so the deref then triggers rule 9.
- **symirsolve:** `is_defined(%p)` conjoined to `PC` at deref.

### Rule 14 — Cross-object pointer comparison
`<`, `<=`, `>`, `>=` between pointers of different originating objects is UB. `==`/`!=` are always defined (distinct objects → distinct addresses → `false`/`true`).

- **symiri:** relational compare on pointers checks both operands share the same `ObjectInfo`.
- **symirc:** UBSan-free territory — the emitted code adds an explicit object-id check before the compare; cross-object compare aborts.
- **symirsolve:** the path condition for a relational compare requires both operands to share a base.

### Rule 15 — Aggregate-derived pointer provenance [revised in v0.2.1]
Every pointer derivation carries a **provenance object**. The rule is uniform between arrays and structs and depends on the *final access*:

- `addr %lv` (top-level local): provenance = `%lv` (the whole local).
- `addr lv.f` / `ptrfield <ptr>, f`: provenance = the **immediate containing struct** of `f`.
- `addr lv[i]` / `ptrindex <ptr>, i`: provenance = the **immediate containing array**.
- `<ptr> ± n`: provenance unchanged from `<ptr>`.

Pointer arithmetic that walks outside the provenance object is UB (rule 10). Type discipline at deref is preserved by rule 15b.

This **relaxes the v0.2.0 rule** ("one-element provenance per scalar struct field"). In v0.2.1, arithmetic within a struct or array is allowed; UB is moved to the deref site where the typed-access check kicks in.

- **symiri:** the `ObjectInfo` for a derived pointer is the *immediate containing aggregate*, not the field/element alone. Arithmetic checks against that aggregate's bounds.
- **symirc:** the emitted code uses the aggregate's `sizeof` for the range check; UBSan `-fsanitize=pointer-overflow` does the boundary trap.
- **symirsolve:** each `addr`/`ptrindex`/`ptrfield` records the provenance (a base address constant + a static byte size); rule 10's path condition uses that pair.

### Rule 15b — Typed-access mismatch [new in v0.2.1]
`load %p` / `store %p, v` through `%p : ptr T` is UB if the runtime address does **not** coincide with the start of a `T`-typed cell within the provenance object. Concretely:

- For a `[N] U` originating object: every address `base + k*sizeof(U)` for `0 ≤ k < N` is a valid `U` cell. (Because the pointer's type necessarily matches the array element type, in-bounds + element-aligned ⇒ valid.)
- For an `@S` originating object: only the offsets of fields with **declared type `T`** are valid cells. A `ptr i32` landing on the offset of an `i64` field — even though arithmetic stayed in `@S`'s bounds — is UB.
- Mid-cell, on a cell of a different type, or straddling cells: all UB.

This is what makes the revised rule 15 type-safe: arithmetic is permissive, but the deref must respect the field types.

- **symiri:** every `load`/`store` walks the provenance object's layout, computes the cell start nearest to the runtime offset, and aborts if the cell's declared type doesn't match the pointer's static type (or the offset isn't aligned to a cell start).
- **symirc:** the emitted load/store is preceded by a layout check generated from the struct's field table (a constant-folded disjunction over valid offsets). UBSan catches the trap path.
- **symirsolve:** at each `load`/`store`, the path condition becomes a disjunction over the valid `T`-typed cell offsets of the provenance object. Symbolic offsets are constrained to one of those values; off-set offsets force `PC := false`.

### Rule 16 — `ptrindex` out-of-bounds [v0.2.1]
`ptrindex <ptr>, <i>` with `<ptr> : ptr [N] T` is UB if `i < 0` or `i > N`. `i == N` produces a valid non-dereferenceable address (good for arithmetic and equality, UB to deref).

- **symiri:** index range check.
- **symirc:** UBSan + emitted range check.
- **symirsolve:** `(bvsle 0 i)` ∧ `(bvsle i N)`.

### Rule 17 — Navigation through `null` [v0.2.1]
`ptrindex <ptr>, <i>` or `ptrfield <ptr>, <f>` is UB if `<ptr>` evaluates to `null`. Caught at navigation, not at later deref, so the path prunes immediately.

- **symiri:** pointer null check at the navigation site.
- **symirc:** emitted pre-navigation null guard; UBSan-trapped on failure.
- **symirsolve:** `(distinct <ptr> (_ bv0 64))` on every navigation.

### Rule 18 — Navigation through `undef` [v0.2.1]
`ptrindex`/`ptrfield` count as reads of their pointer operand; an `undef` operand is UB at the navigation site (consequence of rule 3).

- **symiri:** `is_defined` check on the pointer operand before computing the offset.
- **symirc:** emitted code rejects `undef` pointer values upstream (initialised to `null`, then null-guarded by rule 17).
- **symirsolve:** `is_defined(<ptr>)` conjoined to `PC` at navigation.

### Rule 19 — Navigation from a one-past-the-end pointer [v0.2.1]
`ptrindex <ptr>, <i>` or `ptrfield <ptr>, <f>` is UB if `<ptr>` is exactly the one-past-the-end address of its provenance object — that address is valid for arithmetic and equality, but doesn't point to an element to navigate into.

- **symiri:** runtime check `offset != size_provenance` before each navigation.
- **symirc:** emitted guard alongside the rule-17 null check.
- **symirsolve:** `(distinct offset size_provenance)` conjoined to `PC` at every navigation.

---

## Vector UB (§7.6) [v0.2.1]

### Rule 20 — Out-of-bounds vector lane access
`lv[i]` (read or write) where `lv : <N> T` is UB if `i < 0 ∨ i >= N`.

- **symiri:** lane-index bounds check.
- **symirc:** the emitted code lowers `v[i]` to a subscript on the GCC vector-extension type with an explicit pre-check; UBSan traps on failure.
- **symirsolve:** `(bvult i N)` at each lane access; for symbolic `i` the `ite` chain encoding (§9.5.4) only defines lanes in range, so out-of-range indices force `PC := false`.

### Rule 21 — Lane-wise scalar UB
All scalar UB rules (1–8) apply **per-lane** to vector operations. UB in any single lane prunes the whole path. Examples:

- `%a / %b` where `%a, %b : <4> i32` — UB if any lane of `%b` is 0.
- `%a + %b` where `%a, %b : <4> i32` — UB if any lane overflows.
- `%v as <4> f32` where `%v : <4> f64` — UB if any lane would overflow to ±∞.

- **symiri:** lane-iterates each vector op and checks each scalar rule per lane.
- **symirc:** the emitted SIMD operation is preceded by per-lane checks (extracted via `__builtin_shufflevector` or equivalent), each gated by UBSan.
- **symirsolve:** each lane has its own scalar UB conjunct in `PC`; the entire path is feasible only if every lane's check passes.

### Rule 22 — Reading an `undef` vector lane
Reading a lane whose value is `undef` is UB — either from a vector initialised with `undef`, or from a vector where the read lane has not yet been written by a lane-write or whole-vector copy.

- **symiri:** per-lane `undef` flag tracked alongside lane values.
- **symirc:** emitted vector locals are initialised lane-by-lane to defined values (`0`) where the source uses `undef`; subsequent reads hit defined storage. UBSan handles edge cases.
- **symirsolve:** every lane carries an `is_defined` flag; lane reads conjoin it to `PC`.

---

## Function call UB (§7.7) **[New in v0.2.2]**

### Rule 23 — Contract precondition violation
`call @f(...)` where `@f` is a contract-form `decl`, and any `pre` clause evaluates to `false` at the call site, is UB. The path becomes infeasible.

- **symiri:** contract-form `decl` calls are rejected before execution begins (see "not UB" below), so this rule is never reached.
- **symirc:** the emitted C/WASM checks each `pre` clause before the call; a failed precondition calls `abort()` / `unreachable`.
- **symirsolve:** each `pre` clause is evaluated with arguments bound to parameters. A clause evaluating to `false` conjoins `false` to `PC`, pruning the path.

### Rule 24 — Callee UB propagation
UB encountered during symbolic execution of a `fun` callee makes the **caller's** path infeasible. UB is not sandboxed by call boundaries — if any statement, condition, or nested `call` inside the callee triggers any other UB rule (1–23, 25), the calling path is pruned.

- **symiri:** UB in a callee is a C++ exception that unwinds through the interpreter's call stack; the top-level catches it and terminates.
- **symirc:** the emitted code is monomorphised into a single C/WASM function, so UB in the inlined callee body is caught by the usual UBSan instrumentation. No special cross-function handling needed.
- **symirsolve:** the callee's `PC` is conjoined to the caller's `PC`. If the callee's `PC` becomes `false`, the caller's `PC` also becomes `false`.

### Rule 25 — Intrinsic UB preconditions
Any intrinsic whose declared semantics requires a precondition treats violations of that precondition as UB. This umbrella covers two patterns:

- **Result not representable**: the computed result would overflow the intrinsic's declared return type (e.g., `@abs(INT_MIN_N)`, `@abs_diff(INT_MIN_N, INT_MAX_N)`).
- **Operand-domain restriction**: an operand falls outside the domain the intrinsic is defined on (e.g., `@ctz`/`@clz` require non-zero input; `@ilog2` requires strictly positive input; `@div_euclid` requires non-zero divisor).

The table below enumerates every UB precondition shipped in v0.2.2 batches A through D. Adding a new intrinsic requires (i) declaring its UB preconditions in §12 of `intrinsics.md`, (ii) raising a UB exception in `symiri` when violated, (iii) emitting a guard in `symirc` (C and WASM), and (iv) conjoining the precondition to `PC` in `symirsolve`.

| Intrinsic | UB precondition(s) | Spec |
|---|---|---|
| `@abs(x)` | `x == INT_MIN_N` | §12.1 |
| `@clz(x)`, `@ctz(x)` | `x == 0` | §12.2 |
| `@popcount(x)` | result `> INT_MAX_N` (only triggers for narrow `N`) | §12.2 |
| `@abs_diff(a, b)` | `\|a − b\|` not representable in iN | §12.3 |
| `@clamp(v, lo, hi)` | `lo > hi` (signed) | §12.3 |
| `@rotl(x, n)`, `@rotr(x, n)` | `n < 0` or `n >= N` | §12.4 |
| `@ilog2(x)` | `x <= 0` (signed) | §12.4 |
| `@bswap(x)` | declaration-time: `N % 8 != 0` (rejected at check time, not runtime UB) | §12.4 |
| `@wrapping_shl(x, n)`, `@wrapping_shr(x, n)` | `n < 0` or `n >= N` | §12.5 |
| `@div_euclid(a, b)`, `@rem_euclid(a, b)` | `b == 0` or `(a == INT_MIN_N ∧ b == -1)` | §12.5 |
| `@sqrt(x)` | `x < 0` (NaN result; `@sqrt(-0.0)` is **not** UB) | §12.6 |
| `@from_bits(x)` | bit pattern decodes to a non-finite value (`±∞` or NaN) | §12.6 |
| `@recip(x)` | result non-finite (`x == ±0.0`, or `\|x\|` so small the reciprocal overflows) | §12.6 |

Intrinsics not listed (e.g., `@min`, `@max`, `@signum`, `@midpoint`, `@parity`, `@bitreverse`, `@is_pow2`, the six `@wrapping_*` arithmetic ops, the four `@saturating_*` ops, and the no-UB floating-point intrinsics `@fabs`, `@fneg`, `@copysign`, `@signbit`, `@to_bits`, `@is_normal`, `@is_subnormal`, `@fmin`, `@fmax`, `@floor`, `@ceil`, `@trunc`, `@fract`) have **no UB precondition** — their result is defined for every input in their declared signature.

- **symiri:** every intrinsic implementation in `src/interp/intrinsics.cpp` checks its precondition and throws `UndefinedBehaviorError` on violation.
- **symirc:** the C-backend helper (`src/backend/intrinsics_c.cpp`) emits an `if (cond) __builtin_trap();` guard ahead of the computation; the WASM-backend helper (`src/backend/intrinsics_wasm.cpp`) emits an `if … unreachable end` sequence.
- **symirsolve:** the solver lowering in `src/solver/intrinsics.cpp` pushes the precondition to `pc` so unsatisfying inputs are pruned from the model search.

---

## What's *not* UB in RefractIR

For completeness, a few choices RefractIR deliberately makes well-defined where other languages don't:

- **Equality across objects.** `==`/`!=` between pointers of different originating objects is always well-defined (and always `false`/`true`). Only relational compare is UB (rule 14).
- **One-past-the-end address.** Valid for arithmetic and equality. UB only when dereferenced (rule 11) or navigated through (rule 19).
- **Whole-vector copy.** `%v = %w` for `%v, %w : <N> T` is always well-defined (lane-by-lane copy; no overflow or aliasing concerns).
- **`fmod` semantics for FP `%`.** Aligned with integer `%` (truncate toward zero), not IEEE `fp.rem` (round to nearest even). No UB cases beyond rule 7 (divisor zero → NaN result).
- **Signed `<<` of `x >= 0` whose result fits.** Well-defined arithmetic shift; only `x < 0` or overflow is UB (rule 4).
- **Static call-site checks [v0.2.2].** The following are semantic errors caught before execution, not runtime UB: call to an undeclared function, argument-parameter count/type mismatch, recursion cycle in the call graph, and contract-form `decl` call in `symiri` (which rejects it before execution). These never reach the UB machinery.
- **Argument evaluation UB [v0.2.2].** If an argument expression itself triggers UB (e.g., `call @f(load %null_ptr)`), the UB fires during left-to-right argument evaluation before the call transfers control. This is covered by the existing scalar/pointer/vector UB rules; no new call-specific UB rule is needed.
- **Literal range-check [v0.2.2, SPEC §6.4 + §6.12].** Every integer literal — whether decimal, hex, octal, or binary — is range-checked against the signed two's-complement range `[-2^(N-1), 2^(N-1)-1]` of its inferred type at type-check time. Out-of-range literals are rejected as `StaticError`, not narrowed silently. Examples: `let %x: i8 = 200;` and `let %y: i32 = 0x80000000;` both fail (the values `200` and `2147483648` exceed `INT_MAX_8` and `INT_MAX_32` respectively); authors who intended the bit pattern with the high bit set must write `-128` / `-0x80000000` (or any equivalent signed form). The check applies uniformly to *every* `iN`, including `i1` — the literal `1` in `i1` context is rejected since the representable values of `i1` are `{0, -1}`. This is a static error, not runtime UB, so it does not interact with the UB machinery; symiri / symirc / symirsolve all share the same typechecker pass and reject before any execution begins.

---

## Cross-reference summary

| # | Name | Section in spec |
|---|------|----------------|
| 1 | Integer div/mod by zero | §7.1 |
| 2 | OOB array access | §7.1 |
| 3 | Reading `undef` | §7.1 |
| 4 | Signed overflow | §7.1 |
| 5 | Overshift | §7.1 |
| 6 | FP overflow (±∞) | §7.4 |
| 7 | FP invalid (NaN) | §7.4 |
| 8 | Float→int out-of-range | §7.4 |
| 9 | Null pointer deref | §7.5 |
| 10 | OOB pointer arithmetic | §7.5 |
| 11 | OOB load/store | §7.5 |
| 12 | Cross-object pointer arith | §7.5 |
| 13 | Uninitialised pointer deref | §7.5 |
| 14 | Cross-object pointer compare | §7.5 |
| 15 | Aggregate-derived pointer provenance **[revised v0.2.1]** | §7.5 |
| 15b | Typed-access mismatch **[v0.2.1]** | §7.5 |
| 16 | `ptrindex` OOB **[v0.2.1]** | §7.5 |
| 17 | Navigation through `null` **[v0.2.1]** | §7.5 |
| 18 | Navigation through `undef` **[v0.2.1]** | §7.5 |
| 19 | Navigation from one-past-end **[v0.2.1]** | §7.5 |
| 20 | OOB vector lane access **[v0.2.1]** | §7.6 |
| 21 | Lane-wise scalar UB **[v0.2.1]** | §7.6 |
| 22 | Reading `undef` vector lane **[v0.2.1]** | §7.6 |
| 23 | Contract precondition violation **[v0.2.2]** | §7.7 |
| 24 | Callee UB propagation **[v0.2.2]** | §7.7 |
| 25 | Intrinsic UB preconditions **[v0.2.2]** | §7.7 |
