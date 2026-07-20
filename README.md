# RefractIR

> [!WARNING]
> The project is under active development. APIs and IR details are still evolving, but semantic decisions in `docs/SPEC_v0.2.2.md` are authoritative for the current release (v0.2.2): function calls (`call`), external declarations (`decl`) with link/contract forms, and standard intrinsics.
>
> The version in development is v0.2.3; `docs/SPEC_v0.2.3.md` doubles as its roadmap: native WASM SIMD-128 lowering, the Python target, horizontal reductions, `shuffle`, addressable vectors, vectors in aggregates, and function attributes are designed and tracked there as **[Planned]** (or **[Shipped]** if already implemented).

RefractIR (internally SymIR) is a **CFG-based symbolic intermediate representation** designed for program synthesis, symbolic execution, and constraint generation for SMT solvers using **bit-vector (BV) logic**.

It provides a robust foundation for building tools that need to reason about program semantics, explore execution paths, or synthesize code snippets that satisfy specific properties.

## Table of Contents

- [Core Concepts](#💡-core-concepts)
- [Tools Overview](#🛠️-tools-overview)
- [Getting Started](#🚀-getting-started)
  - [Prerequisites](#prerequisites)
  - [Building](#building)
  - [Usage](#usage)
  - [Switching SMT Backends](#switching-smt-backends)
- [RefractIR Example](#📝-refractir-example)
- [Project Structure](#📁-project-structure)
- [Documentation](#📚-documentation)

## 💡 Core Concepts

- **Path-Oriented:** Designed specifically for symbolic execution and path-based analysis.
- **Symbolic by Design:** Programs may be **symbolic** (contain solver-chosen unknowns marked with `?`) or fully concrete.
- **Explicit Control Flow:** CFG-based representation using basic blocks and `br` instructions (no structured nesting requirements).
- **SMT-Friendly:** Expressions are intentionally restricted to flat, left-to-right forms to ensure predictable BV constraints.
- **Strict Semantics:** Includes **strict undefined behavior (UB)** for operations like division-by-zero or out-of-bounds access, facilitating bug-finding.

## 🛠️ Tools Overview

| Tool | Purpose |
| :--- | :--- |
| `symiri` | **Interpreter**: Execute `.sir` programs directly with concrete values or symbol bindings. |
| `symirc` | **Compiler**: Translate `.sir` programs into optimized C, WebAssembly (WASM), or Python. |
| `symirsolve` | **Solver**: Concretize symbolic programs by solving path constraints via SMT. |
| `rysmith` | **Reifier**: Generate random RefractIR leaf functions for compiler testing. |
| `rylink` | **Reifier**: Generate random RefractIR whole programs for compiler testing. |
| `rytwin` | **Reifier**: Transform a generated program into a semantically-equivalent variant. |

## 🚀 Getting Started

### Prerequisites


We recommend experimenting with RefractIR inside **a docker environment**.

Build the image first:

```bash
docker build -t refractir:latest --build-arg UID=$(id -u) --build-arg GID=$(id -g) .
```

Then start the container:

```bash
docker run -it --rm -v $(pwd):/workspace refractir:latest bash
```

Once inside, skip the prerequisites below and jump directly to the [Building](#building) section.

To run RefractIR in **a local environment** instead, the following tools are required:

- **C++20** compatible compiler (GCC 10+ or Clang 10+)
- **Bitwuzla** (Required by default)
  - Install: https://github.com/bitwuzla/bitwuzla
- **Z3** (Optional)
  - Install: https://github.com/Z3Prover/z3
- **Python 3** (Optional, for the test suite and for running Python-target output)
- **WASM runtime** (Optional, for running WASM backend tests such as Wasmtime, Wasmer, or Node.js)

### Building

To build all tools (interpreter, compiler, and solver):

```bash
make -j$(nproc)
```

To test all tools (interpreter, compiler, and solver):

```bash
make test
```

### Usages

#### Interpret a Concrete Program
```bash
./symiri examples/hello.sir
```

#### Interpret a Symbolic Program with Bindings
Provide concrete values for symbols at runtime:
```bash
./symiri examples/template.sir --sym %?a=10 --dump-trace
```

#### Compile to C, WebAssembly, or Python
```bash
# Compile to C
./symirc input.sir --target c -o out.c

# Compile to WebAssembly
./symirc input.sir --target wasm -o out.wat

# Compile to Python (requires reducible control flow)
./symirc input.sir --target python -o out.py
```

#### Solve for Symbolic Values
Automatically find values for symbols that satisfy any execution path within 100 samples:
```bash
./symirsolve template.sir --sample 100 --require-terminal -o concrete.sir
```

Automatically find values for symbols that satisfy a specific execution path:
```bash
./symirsolve template.sir --path '^entry,^b1,^exit' -o concrete.sir
```

#### Generate Random Programs
```bash
# --emit-desc is required for rylink to generate a program.
./rysmith --emit-desc -n 100
./rylink -n 100
```

#### Emit an Equivalent Twin
```bash
# Generate a program, then emit an equivalent variant. rytwin loads the
# .state.json sidecar when present and otherwise profiles the program
# in-process (no --emit-state needed). Currently, the twin transform does
# not support pointers and memory operations.
./rysmith -n 1 --max-ptr-depth 0 --emit-desc -o out/
./rytwin --p-twin 0.5 --validate -o out/<func>.twin.sir out/<func>.sir
```

### Switching SMT Backends

RefractIR supports multiple SMT solvers via an abstract interface. The following backends are available:

- **Bitwuzla (Default):** Highly optimized for Bit-Vector (BV) and Floating-Point (FP) logic. It is the recommended solver for performance and reliability in symbolic execution tasks.
- **AliveSMT (Z3-based):** A Z3-based backend derived from the Alive2 project. It provides an alternative for verification tasks where Z3's specific heuristics or theories are preferred.

The solver backend is selected at compile-time using the `SOLVER` variable in the `Makefile`.

```bash
# Build with Bitwuzla (default)
make SOLVER=bitwuzla

# Build with AliveSMT (Z3)
make SOLVER=alivesmt
```

## 📝 RefractIR Example

A 4-round substitution-permutation cipher that exercises the full v0.2.2 surface area in one file:

- **Structs + `ptrfield`** — the round state (`@CipherState`) is touched exclusively through typed pointers projected from a single `addr` of the struct.
- **Vectors** — a `<16> i8` S-box and a `<4> i32` round-constant table are held in SIMD registers and indexed per lane.
- **Function calls** — pure helpers `@sbox` (byte→byte) and `@mix` (i32 accumulator update) are invoked from `@main`'s round loop.
- **Overloaded intrinsics** — `@popcount` is declared at both `i8` and `i32` widths; the type checker pins each call site to the correct overload, and the C/WASM backends and the solver all agree. `@rotl(i32, i32)` is the diffusion primitive.

One plaintext byte is symbolic (`%?byte2`); the solver synthesises a value that drives the diffusion accumulator to a precomputed target.

```rust
struct @CipherState {
  s0:    i8;   // byte 0 of the 4-byte block
  s1:    i8;   // byte 1
  s2:    i8;   // byte 2 (initially symbolic)
  s3:    i8;   // byte 3
  acc:   i32;  // diffusion accumulator
  round: i32;  // round counter (0..3)
}

// Overloaded intrinsics — same name, distinct signatures.  The type
// checker pins the chosen overload onto every call site.
intrinsic @popcount(%x: i8)  : i8;
intrinsic @popcount(%x: i32) : i32;
intrinsic @rotl(%x: i32, %n: i32) : i32;

// Pure helper: substitute one byte through a 16-entry SIMD S-box.
fun @sbox(%v: i8) : i8 {
  let %tbl: <16> i8 = {0x6, 0xB, 0x5, 0x4, 0x2, 0xE, 0x7, 0xA,
                       0x9, 0xD, 0xF, 0xC, 0x3, 0x1, 0x0, 0x8};
  let mut %wide:   i32 = 0;
  let mut %nibble: i32 = 0;
  let mut %out:    i8  = 0;
  let %MASK:       i32 = 15;
^entry:
  %wide   = %v as i32;
  %nibble = %wide & %MASK;
  %out    = %tbl[%nibble]; // SIMD lane read
  ret %out;
}

// Pure helper: round mix —  rotl(acc XOR (byte_pop_sum + popcount(rc)), n).
fun @mix(%acc: i32, %byte_pop_sum: i32, %rc: i32, %round_idx: i32) : i32 {
  let mut %pop_rc:  i32 = 0;
  let mut %mix_in:  i32 = 0;
  let mut %xored:   i32 = 0;
  let mut %rotated: i32 = 0;
^entry:
  %pop_rc  = call @popcount(%rc);       // @popcount(i32) overload
  %mix_in  = %byte_pop_sum + %pop_rc;
  %xored   = %acc ^ %mix_in;
  %rotated = call @rotl(%xored, %round_idx);
  ret %rotated;
}

fun @main() : i32 {
  sym %?byte2 : value i8 in [32, 126];  // symbolic plaintext byte
  let %rconsts: <4> i32 = {-0x61C88647, -0x7A143595, -0x3D4D51CB, 0x27D4EB2F};
  let mut %st:  @CipherState = 0;
  let mut %pst: ptr @CipherState = null;
  let mut %p_s0:   ptr i8  = null; let mut %p_s1:   ptr i8  = null;
  let mut %p_s2:   ptr i8  = null; let mut %p_s3:   ptr i8  = null;
  let mut %p_acc:  ptr i32 = null;
  let mut %i:      i32 = 0; let mut %rc:    i32 = 0; let mut %rc_lo: i8 = 0;
  let mut %b0: i8 = 0; let mut %b1: i8 = 0; let mut %b2: i8 = 0; let mut %b3: i8 = 0;
  let mut %p0: i8 = 0; let mut %p1: i8 = 0; let mut %p2: i8 = 0; let mut %p3: i8 = 0;
  let mut %w0: i32 = 0; let mut %w1: i32 = 0; let mut %w2: i32 = 0; let mut %w3: i32 = 0;
  let mut %pop_sum: i32 = 0; let mut %acc: i32 = 0; let mut %r_idx: i32 = 0;
  let %N: i32 = 4; let %ONE: i32 = 1; let %TARGET: i32 = 51328;

^entry:
  // Project typed pointers into the state struct (no aggregate stores).
  %pst   = addr %st;
  %p_s0  = ptrfield %pst, s0; %p_s1 = ptrfield %pst, s1;
  %p_s2  = ptrfield %pst, s2; %p_s3 = ptrfield %pst, s3;
  %p_acc = ptrfield %pst, acc;
  store %p_s0, 0x12 as i8; store %p_s1, 0x34 as i8;
  store %p_s2, %?byte2;    store %p_s3, 0x78 as i8;
  store %p_acc, 0;
  br ^loop_cond;

^loop_cond:
  br %i < %N, ^loop_body, ^check;

^loop_body:
  %rc    = %rconsts[%i];                  // SIMD lane read
  %rc_lo = %rc as i8;
  %b0 = load %p_s0; %b1 = load %p_s1; %b2 = load %p_s2; %b3 = load %p_s3;
  %b0 = call @sbox(%b0); %b1 = call @sbox(%b1);
  %b2 = call @sbox(%b2); %b3 = call @sbox(%b3);
  %b0 = %b0 ^ %rc_lo;    %b1 = %b1 ^ %rc_lo;
  %b2 = %b2 ^ %rc_lo;    %b3 = %b3 ^ %rc_lo;
  %p0 = call @popcount(%b0); %p1 = call @popcount(%b1); // @popcount(i8)
  %p2 = call @popcount(%b2); %p3 = call @popcount(%b3);
  %w0 = %p0 as i32; %w1 = %p1 as i32; %w2 = %p2 as i32; %w3 = %p3 as i32;
  %pop_sum = %w0 + %w1 + %w2 + %w3;
  %acc   = load %p_acc;
  %r_idx = %i + %ONE;
  %acc   = call @mix(%acc, %pop_sum, %rc, %r_idx);
  store %p_s0, %b0; store %p_s1, %b1;
  store %p_s2, %b2; store %p_s3, %b3;
  store %p_acc, %acc;
  %i = %i + %ONE;
  br ^loop_cond;

^check:
  %acc = load %p_acc;
  require %acc == %TARGET, "minicipher hits target";
  ret 0;
}
```

The full annotated source is at [./examples/minicipher_v022.sir](./examples/minicipher_v022.sir).
Find more examples in [./examples](./examples/) and [./test/](./test/).

## 📁 Project Structure

```text
.
├── include/          # Header files
│   ├── ast/          # AST definitions
│   ├── frontend/     # Lexer, Parser, TypeChecker
│   ├── analysis/     # CFG, Dataflow, Dominators/Loops/Structurizer, Pass Manager
│   ├── backend/      # C, WASM, and Python backends
│   ├── solver/       # SMT integration
│   └── reify/        # Reify generators (rysmith/rylink/rytwin)
├── src/              # Implementation files
├── docs/             # Tool and language documentation
├── test/             # Test suite and regression tests
└── Makefile          # Build system
```

## 📚 Documentation

- **[Changelog](./CHANGELOG.md)** — release history from v0.0.1 to the current version.
- **[Language Specification (v0.2.3, current — in progress)](./docs/SPEC_v0.2.3.md)** — the current normative spec and the roadmap for the v0.2.3 line: the Python target (reducible CFGs only), native WASM SIMD-128 vector lowering, completed intrinsic support on WASM, horizontal `@reduce_*` intrinsics, `shuffle`, addressable vectors, vectors in aggregates, and function attributes.
- **[Language Specification (v0.2.2, archived)](./docs/SPEC_v0.2.2.md)** — function calls (`call`), interprocedural execution, external declarations (`decl`) with `-I` link resolution and behavioral contracts (`pre`/`post`/`ret`), and standard intrinsics.
- **[Language Specification (v0.2.1, archived)](./docs/SPEC_v0.2.1.md)** — aggregate pointers (`ptr [N] T`, `ptr @S`, `ptrindex`, `ptrfield`), SIMD vector types (`<N> T`), and `cmp` expressions.
- **[Language Specification (v0.2.0, archived)](./docs/SPEC_v0.2.0.md)** — the pointer baseline (`ptr T`, `addr`, `load`, `store`, `null`).
- **[Language Specification (v0.1.0, archived)](./docs/SPEC_v0.1.0.md)** — the pre-pointer baseline, kept for reference.
- **[Standard Intrinsics reference](./docs/intrinsics.md)** — per-intrinsic signatures, SMT encodings, UB conditions, and C/WASM lowering rules.
- **[Floating-point model](./docs/float.md)** — the finite-only IEEE 754 value model and bit-exact text serialization.
- **[Undefined Behaviour reference](./docs/undefined.md)** — the strict UB model and list of UBs.
- **[CFG Reducibility & Structured Control Flow](./docs/reducibility.md)** — dominator trees, the reducibility check, loop forests, and the control-tree structuring behind the Python target.
- **[symiri User Guide](./docs/symiri.md)**
- **[symirc User Guide](./docs/symirc.md)**
- **[symirsolve User Guide](./docs/symirsolve.md)**
- **[rysmith/rylink/rytwin User Guide](./docs/reify.md)**

## 📋 License

MIT.
