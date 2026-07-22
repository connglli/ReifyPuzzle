# Semantic Reification and Reify

Reify's technique is called *Semantic Reification*, a paradigm for random program generation. Unlike syntactic reification, which operates primarily on syntax, semantic reification centers on program semantics. It distinguishes between two kinds of semantics: compile-time semantics (what a program *can* do) and runtime semantics (what a program *actually does*). The key insight is reformulating random program generation to capture both:

Given an *arbitrary* control flow graph (CFG) $g$ to capture compile-time semantics and an *arbitrary* entry-to-exit path $\pi$ within $g$ (called an execution path or EP) to capture runtime semantics, Reify produces a program $P$, input $i$, and output $o$ satisfying:

1. $P$ is both syntactically and semantically correct for $i$;
2. $g$ corresponds to the CFG of $P$;
3. executing $P(i)$ deterministically follows $\pi$ and produces $o$.

**Why this matters for compiler testing.** Although runtime semantics are fixed for a given input, compilers must reason about all possible executions when optimizing. Semantic reification exposes bugs in that reasoning while guaranteeing every generated program behaves deterministically and is free of undefined behavior *on the specified input*. Allowing arbitrary CFGs and EPs produces complex data flows and diverse control structures, enriching the behaviors available for compiler optimization passes. Compared to existing generators, Reify: (1) inherently supports arbitrary control flow including unbounded loops and irreducible regions; (2) ensures well-definedness and guaranteed termination under the generated input; (3) produces an expected output, enabling direct correctness validation without pseudo-oracles.


## Implementation

Given $g$ and $\pi$, Reify populates each basic block with random statements and jump terminators, then uses *symbolic execution* to derive a path condition and compute an input $i$ that forces $P$ to follow $\pi$ and produce $o$. The symbolic execution explores only the single EP $\pi$, avoiding the path explosion of full symbolic execution.

Reify separates *leaf function generation* (compact functions with no calls) from *whole-program generation* (combining leaf functions into programs with arbitrary call graphs). This document describes the current leaf function generation pipeline and the `rysmith` tool that implements it and the `rylink` whole-program generator.


## Leaf Function Generation

```
S1. CFG Generation   — random control-flow skeleton
S2. Path Sampling    — random entry-to-exit walk through the CFG
S3. Program Seeding  — populate all blocks with typed statements using RefractIR
S4. Concretization   — solve symbolic variables along the EP via SMT
S5. Lowering         — emit concrete RefractIR, then lower to C / WASM
S6. Validation       — compile and execute; compare output to expected
```


### S1: CFG Generation

A random CFG is generated with a configurable number of interior blocks. The structure begins as a spanning chain (entry → b0 → … → b_{n−1} → exit), then stochastically adds branch edges (second successors pointing forward) and back edges (producing loops). The result is always connected with a guaranteed path to exit.

Back edges may land past a loop header and make the CFG **irreducible**. When reducible CFGs are required, the CFG is repaired: retreating edges whose target does not dominate their source are deleted, one per re-analysis pass, so every valid loop survives and only irreducible cycles are broken.


### S2: Path Sampling

An execution path is sampled by a random walk from entry to exit. Back edges are counted per traversal to bound loop iterations. If the walk gets stuck, BFS finds the shortest escape to exit. The path is a sequence of block labels, e.g.:

```
^entry → ^b0 → ^b3 → ^b0 → ^b4 → ^exit
```

The same CFG can yield many distinct paths with different loop iteration counts.


### S3: Program Seeding

This is the core generation step. Every block in the CFG is populated with typed statements using RefractIR. The generation distinguishes two roles:

**On-path blocks** (those appearing in $\pi$): statements use *symbolic variables* whose values will be determined by the SMT solver. Symbols are declared with domains and kind annotations (`coef`, `value`, `index`). Interest constraints — `require` statements that exclude trivial values like 0, 1, −1 from coefficients — push the solver toward diverse, non-degenerate programs.

**Off-path blocks** (those not in $\pi$): statements use *concrete random literals*. These blocks are never executed under the generated input — the solver pins every on-path branch, so control never enters an off-path successor — but the compiler still compiles them. Off-path code is therefore deliberately left unconstrained and may contain UB (division by a variable that could be zero, signed overflow from wide literals, out-of-bounds-capable accesses, etc.). Because off-path code never runs, this UB never reaches the differential oracle; it simply maximizes the diversity of IR presented to optimization passes such as DCE, alias analysis, and vectorization.

Because off-path volume costs the solver nothing, the volume knobs (`--n-stmts`, `--min-atoms`, `--max-atoms`) describe **on-path** blocks, and off-path blocks scale them by `--off-path-multiplier` (default 2×). This buys compiler-facing surface for free and lets on-path volume — the solver's bottleneck — be tuned independently.

#### Type system

Reify uses the full RefractIR type lattice. Each variable independently draws its type from:

| Category | Types |
|---|---|
| Integer scalars | `i8`, `i16`, `i32`, `i64` (and arbitrary `iN`) |
| Floating-point | `f32`, `f64` (disable with `--no-fp`) |
| Arrays | `[N] T` for any element type `T` (depth-bounded) |
| Structs | `@Name { f0: T0; f1: T1; … }` with heterogeneous field types |
| Pointers | `ptr T` for any `T`, including `ptr ptr T` chains |

Mixed types appear within the same function. Scalar type boundaries are crossed with explicit `CastAtom` nodes (sign-extension, truncation, integer-to-float, float-to-integer), which directly test compiler type promotion and narrowing paths.

Floating-point variables are initialized on-path by casting from an integer symbol (`(f32) %?s0`), keeping the SMT problem in BV theory. Off-path float code uses concrete literals.

#### Expression diversity

Expressions are generated *type-directedly*: given a target type `T`, the generator produces an `Expr` of type `T`. All atoms in a single `Expr` share the same type. The atom repertoire includes:

- `coef_sym * var` — linear with symbolic coefficient (on-path)
- `coef_sym & var`, `| var`, `^ var`, `<< var`, `>> var`, `lshr var` — bitwise / shift
- `~var` — bitwise NOT
- `(T) src` — explicit cast from another type
- `load ptr_var` — dereference a pointer variable
- `addr lv` — take the address of a local (produces `ptr T`)
- `select (cond) ? a : b` — lazy ternary (one level deep)
- `coef_sym / concrete_nonzero` — integer division with concrete denominator
- `coef_sym % concrete_nonzero` — integer modulo with concrete denominator

Division and modulo use concrete non-zero denominators on-path (e.g., `%?s3 / 7`), producing div-by-constant patterns that stress compiler strength-reduction. Off-path division uses any concrete literal including zero.

#### Pointer initialization

`addr lv` is an expression atom, not a valid `let` initializer. Pointer variables are therefore declared as `undef` and assigned in the entry block before any other generation:

```sir
fun @func0() : i32 {
  let mut %v0: i32 = %?s0;        // integer var, init from input sym
  let mut %p0: ptr i32 = undef;   // pointer var, init deferred
  let mut %pp0: ptr ptr i32 = undef;  // depth-2 pointer, init deferred
  ...
^entry:
  %p0 = addr %v0;                 // concrete address assignment
  %pp0 = addr %p0;                // ptr ptr chain
  require %?s0 != 0, "nonzero input";
  ...
```

Since `^entry` is always the first block on every path, this guarantees definite initialization for all pointer variables regardless of which path is sampled.

#### On-path coef symbols

Symbolic coefficients are typed to match the expression context. An expression of type `i64` uses a `coef i64` symbol; one of type `i32` uses a `coef i32` symbol. This produces more natural programs (a 64-bit multiply with a 64-bit coefficient) and tests type-specific optimization patterns.


### S4: Concretization

`symirsolve` (or the in-process `SymbolicExecutor` when using `rysmith`) performs path-directed symbolic execution along $\pi$:

1. Executes each on-path block symbolically, collecting:
   - Path conditions from branch terminators
   - `require` constraints (interest constraints, UB guards)
   - Computation results for each assignment
2. Encodes everything as SMT constraints in bitvector theory
3. Calls Bitwuzla to find a satisfying assignment for all symbols
4. Substitutes the model into the program via `SIRPrinter`, emitting a fully concrete `.sir`

The off-path blocks pass through untouched — their concrete literals need no solving.

Multiple concretizations of the same symbolic template (different solver seeds, or re-generation with a different RNG seed) produce structurally similar programs with different numeric values, exploring distinct optimization opportunities from the same control-flow structure.


### S5: Lowering

The concrete `.sir` file is lowered to C or WASM by `symirc`:

```
rysmith  →  concrete .sir  →  symirc -t c  →  .c  →  gcc / clang (link with -lm)
                            →  symirc -t wasm →  .wat / .wasm
```

The generated C code is suitable for direct compilation and execution under the generated input $i$. The expected output $o$ is the return value of the function (the checksum over all live variables at exit).


### S6: Validation

The generated program is compiled with the target compiler and executed under $i$. If the output differs from $o$, Reify reports a potential miscompilation.

```
Expected:  func0() = -847
Compiled (-O3):  func0() = -846   → POTENTIAL BUG
```

Differential testing across compiler versions or optimization levels is also supported.


## Whole-Program Generation

The leaf generation pipeline (S1–S6) produces independent functions. To build a complete program, Reify generates a random call graph (CG) and applies *semantics-preserving peephole rewriting*: a constant `c` in a caller is replaced with `f(i) + (c − o)`, where `f(i) = o`. This establishes an inter-procedural call while preserving the constant's value at runtime.

Whole-program generation is implemented by `rylink`, described below. The pipeline:

```
W1. Pool ingest        — load a directory of rysmith-emitted (.sir + .json) pairs
W2. CG generation      — pick K functions and build a DAG call graph over them
W3. Bundle merge       — parse each .sir, union into one Program (dedup structs by name)
W4. Peephole rewrite   — for each (caller, callee) edge, splice `call @callee(args) + (c − o)`
W5. Lowering           — emit program.sir + optional symirc --split-by-source C/WASM
W6. Validation         — symiri runs the bundled entry with its solved params; check return
```

Each chosen leaf function brings its own solved realization (one of the `--n-inits` rysmith concretizations) so the rewrite expression `call + (c − o)` is semantically equivalent to the original literal at runtime. The call-realization transform (`CallRealizeTransform`, a whole-program `Transform`) consumes each rewrite site at most once across the entire program; composing two rewrites on the same literal would produce a left-to-right call chain (`f1() + f2() + …`) whose prefix sums can wrap in unintended ways even though each individual rewrite is BV-sound.

## Twin-Program Generation

A twin program of a given program is its equivalent variant. Twin-program generation is based on leaf functions, too.

Given a leaf function `f1` together with the exact input `i` that concretizes it, the whole execution is deterministic and known. `rytwin` obtains, for each on-path program point, the concrete value of every initialized local/parameter — the state the program passes through — from the `.state.json` sidecar when `f1` was generated with `rysmith --emit-state`, and otherwise by interpreting `f1` on `i` in-process. For a chosen basic block `B`, let `s` be the state at `B`'s entry and `s' = B(s)` the state at its exit. Twin-program generation synthesizes a **twin block** `B'` whose net effect from `s` is exactly `s'` and grafts a guarded diamond:

```
^X  (guard):  br call @__twg_<fn>_<X>(<state>) != 0  ->  ^X__twin  else  ^X__orig
^X__twin:     B'   ->  ^X__merge      (reproduces B's effect at s)
^X__orig:     B    ->  ^X__merge      (the original block body)
^X__merge:    <B's original terminator>
```

The guard fires only when the live-in state equals `s`, so on the profiled input the twin runs (producing exactly what `B` would) and on every other state the original runs — hence full equivalence. The guard is a generated function `@__twg_<fn>_<label> : i1`, one per twin site. It consumes the **entire** definitely-initialized state at `B`'s entry — not only `B`'s read set — as a conjunction of per-leaf equalities. The conjunction is total (no UB) and collision-free, so it preserves the equivalence on all inputs, not just the profiled one, and it cannot fire on any state other than `s`. Scalar roots cross into the guard by value, vector roots per-lane, and aggregate roots by address (`ptr [N] T` / `ptr @S` parameters, navigated inside the guard with in-bounds `ptrindex`/`ptrfield` + `load`). Each candidate block is twinned with probability `--p-twin`.

`B'` is generated the same way rysmith generates blocks: random statements with `%?` symbols over the live state (UB-safety `require`s spliced automatically), one fresh additive correction symbol per touched leaf so any target stays reachable, and one equality `require` per leaf pinning the final state to `s'`. The resulting single-block mini-program is solved in-process with the SMT solver, concretized by printing with the model and re-parsing, and then verified **bit-exactly** by re-running the interpreter (the solver's FP equality is IEEE and would conflate `+0.0`/`-0.0`). The scaffolding equality requires are stripped from the graft; the UB-safety requires remain (they hold on the guarded state). On UNSAT/timeout the attempt is retried with fresh statements (`--twin-retries`), and when every attempt fails — or with `--no-twin-smith` — `B'` falls back to a constant reconstruction of the leaves `B` writes.

**Pointers and memory**. The state profile records each pointer leaf's provenance — the originating local and the byte offset of the pointee cell — so memory-op blocks (`load`/`store`/`addr`/`ptr`-navigation) are twin candidates like any other. A block's effect is the bit-exact state diff `s -> s'` (store-through-pointer effects surface as diffs of the pointee root), changed pointer leaves are reconstructed with `addr <root>[path]` / `null`, and the guard compares pointer values with `==` against caller-reconstructed expected pointers (equality is defined across objects, so the check is total). Memory-op blocks require the entire frame state to be guardable — a load can observe any root through a pointer.

**Limitations**. Blocks containing non-intrinsic calls are not twinned (a callee handed a pointer into an outer frame could mutate state the frame diff does not see), and solver-generated twin bodies currently fall back to constant reconstruction when the block's state contains pointer leaves.

## Tool: rysmith

`rysmith` implements S1–S5 in a single in-process C++ binary. It builds RefractIR program ASTs directly in memory, calls `SymbolicExecutor` in-process (no subprocess), and emits concrete `.sir` files via `SIRPrinter`. It can optionally invoke `symiri` for S6 validation. The main focus is function generation. It does not test the compilers directly.

### Usage

```
rysmith [OPTIONS]
```

### Options

#### Type control

| Flag | Default | Description |
|---|---|---|
| `--no-fp` | off | Disable `f32`/`f64` types entirely |
| `--max-ptr-depth N` | 2 | Maximum pointer nesting depth (`ptr ptr T` = depth 2) |
| `--max-agg-nest N` | 2 | Maximum aggregate nesting depth |
| `--max-agg-elems N` | 3 | Maximum array size and struct field count |

#### Generation

| Flag | Default | Description |
|---|---|---|
| `--n-vars N` | 10 | Total variables per function (types drawn independently) |
| `--n-stmts N` | 3 | Statements per on-path block |
| `--off-path-multiplier F` | 2.0 | Scale `--n-stmts` / `--min-atoms` / `--max-atoms` by `F` in off-path blocks |

#### Operators

| Flag | Default | Description |
|---|---|---|
| `--no-divmod` | off | Disable integer division and modulo |
| `--no-select` | off | Disable `select` ternary expressions |

#### CFG

| Flag | Default | Description |
|---|---|---|
| `--n-bbls N` | 15 | Basic blocks between entry and exit per CFG |
| `--p-branch F` | 0.5 | Probability of a two-successor (branch) block |
| `--p-backedge F` | 0.3 | Probability of a back edge (loop) from a non-entry/exit block |

#### Solver

| Flag | Default | Description |
|---|---|---|
| `--timeout N` | 2000 | SMT solver timeout per attempt (ms) |
| `--seed N` | random | Master RNG seed |
| `--require-ub` | off | Generate programs that **trigger** UB on the sampled path instead of UB-free ones (see below). Implies `--no-crc32`. |
| `--no-crc32` | off | Keep the sum-form checksum (`%_chk = %_chk + <leaf>`) in the emitted program instead of rewriting it to `@crc32_update` calls |

#### Output

| Flag | Default | Description |
|---|---|---|
| `-n, --n-funcs N` | 1 | Number of leaf functions to generate |
| `--n-inits N` | 3 | Concretizations per CFG+path template |
| `--max-loop-iter N` | 1 | Max iterations of any single loop in the sampled path |
| `--min-loop-iter N` | unset | If set, force at least one loop in the path to iterate ≥ N times (rejects loop-free CFGs) |
| `--max-retries N` | 2 | Retry attempts on solver failure (simpler path each time) |
| `-o, --output-dir PATH` | `reify_out` | Output directory for `.sir` files |
| `--target sir\|c\|wasm\|python` | `sir` | Optionally compile each concrete `.sir` in-process (`python` implies `--require-reducible`) |
| `--require-reducible` | off | Only generate reducible CFGs (irreducible back edges are repaired away) |
| `--structured-lowering true\|false\|random` | `false` | Structured lowering for the C (goto-free) and WASM (dispatch-free) targets, resolved per program; `true`/`random` imply `--require-reducible` |
| `--vec-lowering <s>` | `random` | Vector lowering strategy, resolved per program; `random` sweeps the target's set (C: all five; python: all but `vecext`) |
| `--keep-require` | off | Include `require` checks in compiled output |
| `--keep-ub-guards` | off | Keep the backends' dynamic UB guards in compiled output even for UB-free programs. By default UB-free generation (i.e. without `--require-ub`) drops them — see below |
| `--keep-symbolic` | off | Write intermediate symbolic `.sir` to disk |
| `--validate` | off | Run `symiri` on each concrete `.sir` and check its `Result:` line matches the descriptor's captured CRC32 retValue |
| `--emit-main` | off | Append a `@main()` wrapper that calls the entry with its solver-synthesised params and asserts the CRC32 retValue via `@check_chksum` |
| `--emit-desc` | off | Emit per-function descriptor JSON (`func_<id>_<i>.json`) used by `rylink`; records a `reducible` bool computed from the emitted function so structuring consumers can filter seeds, and a `has_ub` bool (true under `--require-ub`) so `rylink` knows whether the leaf's UB guards can be dropped |
| `--emit-state pbb\|ppp` | off | Emit a `func_<id>_<i>.state.json` profile of the concrete state at each program point (`pbb` = per basic-block entry, `ppp` = per program point) — loaded by `rytwin` when present, sparing it the in-process profiling run |
| `-v, --verbose` | off | Verbose progress output |

### Example

```sh
# Generate 10 diverse functions, 3 concretizations each, validate all
rysmith -n 10 --n-inits 3 --validate -o out/

# Stress pointer and mixed-type generation, disable floats
rysmith -n 20 --no-fp --max-ptr-depth 2 --max-agg-nest 2 -o out/

# Reproduce a specific run
rysmith -n 30 --seed 42 -o out/
```

### Output format

Each concrete `.sir` file is a valid RefractIR program containing one function `@funcN`. All variables are initialized to concrete integer or float values. The `^exit` block folds **every** scalar leaf of every let-init local and every parameter — recursing through nested arrays, structs, and vector lanes — into a running CRC32 state and returns it:

```sir
intrinsic @crc32_update(%state: i32, %val: i32) : i32;

fun @func0(%pa0: i32) : i32 {
  let mut %v0: i32 = 7;
  let mut %v1: i64 = -3;
  let mut %p0: ptr i32 = undef;
  let mut %_chk: i32 = 0;
^entry:
  %p0 = addr %v0;
  ...
^exit:
  %_chk = 0;
  %_chk = call @crc32_update(%_chk, %v0);
  %_chk = call @crc32_update(%_chk, %v1 as i32);
  %_chk = call @crc32_update(%_chk, %pa0);
  ret %_chk;
}
```

The return value is the expected output $o$. Internally rysmith asks the solver for the cheaper sum-based contract (`%_chk = %_chk + atom`), then a post-solve rewriter replaces every accumulator step with a `@crc32_update` call before the .sir is written; the solver never has to encode the CRC32 recurrence. After lowering to C with `symirc -t c`, executing the function should always return this value regardless of compiler version or optimization level — the helper carries a function-local `static` lookup table and a `static __attribute__((noinline))` qualifier (see `docs/intrinsics.md` §12.7) so the optimizer cannot fold the chain.

With `--emit-main`, rysmith additionally appends a `@main()` wrapper that calls `@func0` with the solver-synthesised parameter values and asserts the return matches the captured CRC32 via `@check_chksum(EXPECTED, %r);`. The C lowering of `@check_chksum` aborts on mismatch (`fprintf(stderr, …); abort();`) — that externally-visible side effect anchors the entire call chain against IPA-CP, so the compiler cannot fold the body away even at `-O3 -flto`.

### Generating UB-triggering programs (`--require-ub`)

By default every generated program is UB-free on its input: the solver asserts each operation's safety guard, so the concretization executes cleanly and returns the checksum. With `--require-ub`, rysmith instead asks the solver to **negate** the conjunction of those guards (delegated to `symirsolve`'s RequireUB mode — see [symirsolve.md](./symirsolve.md)), so the concretization is guaranteed to trigger at least one UB on the sampled path. This is used to exercise the UB-detection of downstream tools.

`--require-ub` **implies `--no-crc32`.** The solver reasons about the *sum-form* checksum (`%_chk = %_chk + <leaf>`, the cheap contract above), and one legitimate way to satisfy "at least one UB on the path" is to overflow that signed accumulator. The post-solve CRC32 rewriter, however, replaces every `%_chk = %_chk + <leaf>` with a total `@crc32_update(...)` call — which cannot overflow — so it would silently *delete* the very UB the solver just proved, leaving the emitted program UB-free. Keeping the sum form (`--no-crc32`) makes the program rysmith emits byte-identical to the one it solved, so a solver-found UB is guaranteed to trap under the interpreter. This costs nothing: a UB-triggering program aborts before it reaches a clean `ret`, so its CRC32 return-value oracle is vestigial anyway.

### Dropping UB guards for UB-free output

The C/WASM/Python backends emit dynamic UB guards (`symirc --no-ub-guards`; see [symirc.md](./symirc.md#omitting-ub-guards---no-ub-guards-v023)). Because those guards only ever fire on a UB path, a program the reify pipeline proves UB-free renders them dead weight, so the tools **drop them automatically** rather than exposing a flag:

- **rysmith** drops the guards whenever it is not in `--require-ub` mode, and records `has_ub` in each `--emit-desc` descriptor accordingly.
- **rylink** drops them only when *every* selected pool leaf has `has_ub: false` — a bundle is UB-free iff all its leaves are. Legacy descriptors without the field parse as `has_ub: true`, so their guards are conservatively kept.
- **rytwin** drops them unconditionally: a twin is equivalence-preserving over UB-free input, and the interpreter it profiles `p1` with would itself fail on any UB.

Each tool takes **`--keep-ub-guards`** to force the guards back on — useful for catching a mislabeled UB-free program that *does* trigger UB, which then traps at runtime instead of silently misbehaving.


## Tool: rylink

`rylink` reads a rysmith function pool, builds whole programs over it, and (optionally) compiles and validates each one following W1-W5.

When `--structured-lowering` is `true`/`random` — or the target is `python` — seed programs may not be reducible (older pools, or runs without `rysmith --require-reducible`), so rylink **discards every pool seed whose descriptor's `reducible` flag is false** before generation (descriptors predating the flag parse as false and are conservatively discarded too). If no reducible seeds remain, rylink aborts with a pointer to `rysmith --require-reducible`. The composed program is then reducible by construction: every inlined seed is, and the generated `@main` wrapper's CFG is trivial.

### Usage

```
rylink [OPTIONS]
```

### Options

| Flag | Default | Description |
|---|---|---|
| `-i, --input-dir PATH` | `rysmith_out` | Directory of rysmith-emitted `(.sir + .json)` pairs (`rysmith --emit-desc`) |
| `-o, --output-dir PATH` | `rylink_out` | Root; each program lands in `<root>/prog_<id>_<i>/` |
| `-n, --n-progs N` | 1 | Number of whole programs to generate |
| `--id HEX6` | random | 6-hex-char generation ID prefix |
| `--seed N` | random | RNG seed |
| `--n-nodes N` | 4 | Target number of call-graph nodes per program |
| `--max-outdeg N` | 3 | Maximum out-degree per CG node |
| `--target sir\|c\|wasm\|python` | `c` | `c` uses `symirc --split-by-source`; `python` emits a single `program.py`; `sir` skips lowering |
| `--structured-lowering true\|false\|random` | `false` | Structured lowering for the C (goto-free) and WASM (dispatch-free) targets, resolved per program |
| `--vec-lowering <s>` | `random` | Vector lowering strategy, resolved per program from the target's set (C: all five; python: all but `vecext`) |
| `--keep-require` | off | Keep `require` checks in C/WASM output |
| `--keep-ub-guards` | off | Keep the dynamic UB guards even when the bundle is UB-free (default: dropped — see *Dropping UB guards* above) |
| `--validate` | off | Run `symiri` on each emitted program and assert the entry returns its descriptor's solved value |
| `-v, --verbose` | off | Per-init log lines (`validated: OK`, `symirc FAIL`, etc.) |

### Output layout

Each program lives in its own subdirectory:

```
rylink_out/
  prog_<id>_0/
    program.sir        # bundled RefractIR (header comments: ENTRY, CG, PARAMS, RETURN)
    common.h           # symirc --split-by-source artefacts (when --target c)
    program.c
  prog_<id>_1/
    ...
```

The bundled `.sir` is the source of truth for every downstream consumer. Header comments record the entry function, the call graph, the solved parameter values for the entry, and the expected return value — making each bundle reproducible without consulting the descriptor JSON.

### Example

```sh
# 1. Build a pool of 200 leaf functions with descriptors
rysmith -n 200 --emit-desc -o pool/

# 2. Generate 10 whole programs of ~4 functions each, validate every one
rylink -n 10 --n-nodes 4 --validate -i pool/ -o progs/

# 3. C target with require checks kept
rylink -n 5 --target c --keep-require -i pool/ -o progs/

# 4. Structured (goto-free) C over a reducible pool
rysmith -n 200 --emit-desc --require-reducible -o pool/
rylink -n 5 --target c --structured-lowering random -i pool/ -o progs/
```

## Tool: rytwin

`rytwin` is an **equivalence-preserving program transformer**. Given a generated program `f1` (a rysmith leaf or a rylink whole program), it emits an equivalent program `f2` such that `f1(i) == f2(i)` for **every** input `i` — same result, same undefined-behaviour outcome. Whole programs are profiled from `@main`, and twins are grafted into any function along the executed trace; the state capture is frame-aware, so states are attributed to the right activation even when block labels repeat across functions.

### Usage

```sh
rytwin <f1.sir> [OPTIONS]
```

The descriptor (`func_<id>_<i>.json`) and, when present, the state profile (`<stem>.state.json`) are read from `f1`'s directory following rysmith's naming, so only `f1` is passed positionally. Without a sidecar the profile is computed in-process: rytwin interprets `f1` on its solved input (the descriptor realization, or `f1`'s `// SOLVED:` header when no descriptor is present).

| Flag | Default | Description |
|---|---|---|
| `-o, --output PATH` | — | Output `.sir` (`f2`) |
| `--p-twin P` | 0.5 | Probability of grafting a twin for each candidate block |
| `--no-twin-smith` | off | Disable rysmith-style twin generation; reconstruct the post state with constants |
| `--twin-stmts N` | 3 | Random statements per generated twin |
| `--twin-retries N` | 3 | Generation attempts per twin before falling back |
| `--seed N` | random | RNG seed |
| `--target sir\|c\|wasm` | `sir` | Optionally compile `f2` via the in-process backend |
| `--validate` | off | Run `symiri` on `f1` and `f2` with the profiled input, assert they agree, and assert at least one twin block actually executed |
| `--keep-require` | off | Keep `require` checks in compiled output |
| `--keep-ub-guards` | off | Keep the dynamic UB guards in the compiled twin (default: dropped — the twin is assumed UB-free; see *Dropping UB guards* above) |
| `--emit-main` | off | Keep `@main` un-mangled in compiled output |

### Example

```sh
# 1. Generate a program (pointer-free here, so more blocks are twin-eligible)
rysmith -n 1 --emit-desc --emit-main --max-ptr-depth 0 -o out/

# 2. Emit an equivalent twin, twinning every eligible block, and self-validate
rytwin out/func_<id>_0.sir --p-twin 1.0 --validate -o out/twin.sir

# 3. Differential test: compile both and compare
rytwin out/func_<id>_0.sir --p-twin 1.0 --target c --emit-main -o out/twin.sir
```

## Known Issues

The following commits, together, cause a 3-5x rysmith performace degradation:

1. 14343fc completely removed trivial "lit op lit" atoms.
2. e390437 excluded store statements from counting into --n-stmts.
3. 7118748 introduced indirect store and load.

Limiting them would bring back some trivial patterns that might not be
bad for compiler testing, and would allow more performant generation.
