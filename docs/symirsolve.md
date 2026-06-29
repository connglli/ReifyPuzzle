# symirsolve — RefractIR Concretizer (SMT-based)

`symirsolve` concretizes a **symbolic** RefractIR (`.sir`) program into a **concrete** RefractIR program by solving
constraints with an SMT solver over **bit-vectors (BV)**.

Concretization means:
- assign concrete values to all declared symbols (`@?x`, `%?y`),
- optionally specialize to a particular execution path,
- and emit a `.sir` program with symbols replaced (or with an explicit symbol-value section).

`symirsolve` is the bridge between synthesis/verification constraints and runnable code.


## Goals

- Concretize symbolic templates into concrete RefractIR programs
- Support path-based constraint extraction (symbolic execution along a user-specified path)
- Provide deterministic, reproducible models (optional: seed / model selection policy)
- Produce outputs that can be:
  - interpreted by `symiri`, or
  - translated by `refractirsolvec`


## Usage

```bash
symirsolve <input.sir> [--path <labels> | --sample <n>] [options]
```

### Common Examples

Concretize using constraints embedded in the program (`assume`, `require`) and a specified path (defaults to `@main`):

```bash
symirsolve template.sir --path '^entry,^b1,^b3,^b1,^b2,^exit' -o concrete.sir
```

Concretize by randomly sampling up to 100 paths until a SAT one is found:

```bash
symirsolve template.sir --sample 100 --require-terminal -o concrete.sir
```

Use multi-threading to speed up sampling (2 threads):

```bash
symirsolve template.sir --sample 100 --require-terminal -j 2 -o concrete.sir
```

Use all available CPU cores for sampling:

```bash
symirsolve template.sir --sample 100 --require-terminal -j 0 -o concrete.sir
```

Concretize and also emit a model file:

```bash
symirsolve template.sir --path '^entry,^b1,^b3,^b1,^b2,^exit' --emit-model model.json -o concrete.sir
```

Concretize with additional symbol constraints provided on the command line:

```bash
symirsolve template.sir --path '^entry,^b1,^b3,^b1,^b2,^exit' --sym %?c4=3 -o concrete.sir
```


## Inputs and Constraint Sources

`symirsolve` derives constraints from:

1. **Symbol declarations** (`sym`) and their domains (`in [lo,hi]` or `in {…}`)
2. **Path constraints** induced by the chosen path:

   * `br cond, ^t, ^f;` contributes `cond` if the path takes `^t`, else `not(cond)`
3. **`assume` constraints** (feasibility constraints)
4. **`require` constraints** (property constraints that must hold)

Strict UB is enforced:

* Any UB encountered on the chosen path (e.g., division by zero or OOB) makes that path infeasible.


## Path Specification

A path is specified as a sequence of block labels, including repeats:

```text
^entry,^b1,^b3,^b1,^b2,^exit
```

Rules:

* The sequence must be compatible with `br` edges in the CFG.
* `symirsolve` uses the path to select which side of conditional branches is taken.
* If `--sample` is used, the path acts as a mandatory **prefix** for all sampled traces.


## Random Path Sampling

Instead of providing a complete path, `symirsolve` can explore the CFG automatically:

- **`--sample N`**: Performs up to $N$ random walks starting from the function entry (or the `--path` prefix).
- **Early Exit**: The process stops as soon as the first logically feasible (`SAT`) path is found.
- **`--require-terminal`**: If a random walk reaches `--max-path-len` without hitting a `ret`, `symirsolve` will attempt to complete the trace using the shortest path to any `ret` block. If disabled, non-terminating samples are discarded.


## Multi-Threading Support

`symirsolve` supports two types of parallelism:

### 1. Path Sampling Parallelism (`-j, --num-threads`)

Controls how many parallel threads explore different paths simultaneously:

- **`-j N` or `--num-threads N`**: Use `N` threads for parallel path sampling
- **`-j 0`**: Automatically use all available CPU cores (determined by `std::thread::hardware_concurrency()`)
- **Default**: Single-threaded execution (`-j 1`)

Multi-threading is most effective with:
- **Large sample counts** (`--sample` with high values)
- **Complex search spaces** (multiple symbolic variables, many paths)
- **Non-deterministic solving** (where different seeds/paths may have different solve times)

**Backend-Specific Limitations:**
- **Bitwuzla**: Fully supports multi-threading. Each thread creates an independent solver instance.
- **AliveSMT (Z3)**: Does **NOT** support multi-threading due to Z3's global context being non-thread-safe.
  - If `-j > 1` is specified with AliveSMT, `symirsolve` will automatically fall back to single-threaded execution with a warning.
  - This is a limitation of Z3's global state management and reference counting.

**Implementation Notes:**
- Each thread uses an independent solver instance with a different random seed (based on the base `--seed` + thread ID)
- The first thread to find a SAT result causes all threads to terminate early
- Thread-safety is ensured through proper synchronization of shared state

### 2. SMT Solver Internal Parallelism (`--num-smt-threads`)

Controls how many threads each SMT solver instance uses internally:

- **`--num-smt-threads N`**: Configure the SMT backend to use `N` threads for parallel solving
- **Default**: Single-threaded SMT solving (`--num-smt-threads 1`)

**Backend-Specific Behavior:**
- **Bitwuzla**: Sets the `NTHREADS` option for parallel constraint solving
- **AliveSMT (Z3)**: Sets the `sat.threads` parameter for parallel SAT solving

**When to Use:**
- **Use `-j N`** when you want to explore multiple paths in parallel (different symbolic executions)
- **Use `--num-smt-threads N`** when you want each individual SMT query to be solved faster using parallelism
- **Combine both** for maximum performance: `-j 4 --num-smt-threads 2` uses 4 path exploration threads, each with a 2-thread SMT solver


## Outputs

* **SAT**: If a solution exists, `symirsolve` reports `SAT`.
* **Concrete SIR**: If `-o <file>` is specified, it produces a concrete `.sir` where all symbols are replaced with concrete constants.
* **Model File**: If `--emit-model <file>` is specified, it produces a JSON file mapping the entry function to its solved symbol values.
* **AST Dump**: If `--dump-ast` is specified, it prints the internal AST representation of the concretized program to stdout.


## Options

| Option                | Description                                              |
| --------------------- | -------------------------------------------------------- |
| `--main <func>`       | Function to concretize (default: `@main`)                |
| `--path <labels>`     | Comma-separated block labels (acts as prefix if sampling)|
| `--sample <n>`        | Number of paths to sample randomly until SAT is found    |
| `--max-path-len <n>`  | Maximum random path length (default: 100)                |
| `--require-terminal`  | Force paths to reach 'ret' via shortest path if needed   |
| `-j, --num-threads <n>` | Number of threads for parallel path sampling (0 = use all available CPU cores, default: 1) |
| `--num-smt-threads <n>` | Number of threads for SMT solver internal parallelism (default: 1) |
| `-o <file>`           | Output concrete `.sir` file                              |
| `--dump-ast`          | Dump concretized AST to stdout                           |
| `--timeout-ms <n>`    | Solver timeout in milliseconds                           |
| `--seed <n>`          | Seed for deterministic model selection                   |
| `--emit-model <file>` | Emit symbol assignments in nested JSON format            |
| `--sym sym=val`       | Fix a symbol to a concrete value before solving          |
| `-h, --help`          | Print usage                                              |


## Notes on BV Semantics

`symirsolve` encodes integer types as BV sorts:

* `i32` → `(_ BitVec 32)`
* `i64` → `(_ BitVec 64)`
* `iN`  → `(_ BitVec N)` (backend support may vary)

Operators:

* `/` uses signed truncating division: `bvsdiv`
* `%` uses signed remainder: `bvsrem`

These match the language semantics in `docs/SPEC_v0.2.2.md`.


## Notes on Pointer Support (v0.2.2)

The solver encodes pointers as **64-bit BV tags** identifying the addressed storage (tag `0` reserved for `null`; the base tag is the FNV-1a hash of the local name, and sub-object addressing advances it by a *leaf-unit* offset — each scalar or pointer leaf counts as one unit and arrays/structs sum their leaves, per `sizeofTagUnits`; this is **not** a byte offset). `addr` / `load` / `store` dispatch builds an `ite`-chain over candidate targets of the matching pointee type, so load/store through symbolic pointers resolves to the right cell without requiring SMT array theory.

**Currently supported in the solver:**

* `addr %v` for a whole `let mut` local `%v` (scalar or pointer).
* `addr %arr[i]` and `addr %st.f` — array-element and struct-field addressing (the v0.2.1 §9.4 provenance model: base tag + leaf-unit offset, provenance = the immediate containing aggregate, with rule-15b typed-access checks at the deref).
* `ptrindex` / `ptrfield` aggregate navigation, including one-past-the-end and the associated navigation UB (rules 16–19).
* Pointer arithmetic (`ptr ± iN`, `ptr - ptr`) with in-bounds / cross-object UB enforced via per-provenance base+size constraints.
* `load %p` / `store %p, v` where `%p` is a `ptr T` value that ultimately points at a `T`-typed cell; pointer-to-pointer chains (`ptr ptr T`).
* Pointer equality / inequality (`==`, `!=`) on the 64-bit tags; relational compare (`<`, `<=`, `>`, `>=`) is UB across distinct objects (rule 14).
* Pointer **parameters** of functions, threaded through interprocedural `call` (§9.6) with caller-store coherence on callee `store`s.

**Known limitations (v0.2.2):**

* `sym` of pointer type is not supported (SPEC §13 non-goal — pointer symbols need a richer address-domain theory).
* Contract-form `decl` memory havoc currently resolves only the `addr %x` and plain-ptr-local argument forms; aggregate / `ptrindex` / `ptrfield` / derived / nested-pointer provenance is not yet havoc'd (SPEC §9.6.2 step 4 and the §13 "Contract memory havoc — extended provenance forms" non-goal). Callers passing those forms must constrain the post-state explicitly.
* Branchy callees use seeded random sub-path sampling (no user-supplied callee sub-path syntax yet — SPEC §9.6.4 / §13).

The BV-tag model gives sound, fast symbolic execution without SMT array theory: because each scalar leaf is a single addressable unit, aliasing and pointer arithmetic resolve through `ite`-chains over candidate cells rather than a byte-addressable `Mem[T]`. This handles the full typed-pointer fragment (aggregate addressing, navigation, and in-bounds arithmetic) but deliberately cannot model byte-level reasoning — which is why the byte-granular memory intrinsics (`@memcpy`, `@memset`) remain a spec §13 non-goal. The three limitations listed above are the remaining deltas from the abstract spec §9.4 model.


## Failure Modes

| Result            | Meaning                                                |
| ----------------- | ------------------------------------------------------ |
| SAT               | Concretization succeeded; output program is emitted    |
| UNSAT             | No assignments satisfy constraints for the chosen path |
| UNKNOWN / TIMEOUT | Solver could not determine satisfiability              |

`symirsolve` should always report the solver status with a clear diagnostic.


## Relationship to Other Tools

* `symirsolve` produces concrete `.sir` that `symiri` can execute
* `symirc` can translate the concretized `.sir` into C/WASM
* `symirc` can also translate symbolic `.sir` directly (using extern/import symbol hooks), but `symirsolve` is the tool that actually solves and fixes symbol values
