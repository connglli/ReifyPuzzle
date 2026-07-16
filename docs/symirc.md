# symirc — RefractIR Translator

`symirc` translates RefractIR (`.sir`) programs into **C**, **WebAssembly**, or **Python**.

It supports both:
- **concrete programs**, and
- **symbolic programs**, where symbols are emitted as external declarations/imports.

`symirc` does not solve constraints; it preserves symbolic structure for external integration.


## Goals

- Provide first-class translation to C, WebAssembly, and Python
- Preserve precise RefractIR semantics
- Allow symbolic programs to be linked with external providers of symbol values
- Act as a backend-independent lowering stage for future targets


## Usage

```bash
symirc <input.sir> --target <c|wasm|python> [-o output]
````

### Examples

Translate to C:

```bash
symirc prog.sir --target c -o prog.c
```

Translate to WebAssembly (text format):

```bash
symirc prog.sir --target wasm -o prog.wat
```

Translate to Python (requires reducible control flow, see below):

```bash
symirc prog.sir --target python -o prog.py
```


## Symbolic Programs

If the input program declares symbols (`sym`), `symirc` emits **external symbol hooks**.

### C target

Each symbol is translated into a zero-argument external function:

```c
extern int32_t f0__c4(void);
```

Symbol references become calls:

```c
f0__c4()
```

### WebAssembly target

Each symbol becomes an imported function:

```wat
(import "f0" "c4" (func $f0__c4 (result i32)))
```

Symbol references become:

```wat
(call $f0__c4)
```

### Python target

Each symbol becomes a call to a **provider function** that the module
itself does not define:

```python
f0__c4()
```

The embedding must inject the providers into the module's globals
before invoking the entry function (this is what the test driver
does):

```python
import prog
prog.f0__c4 = lambda: 7
print(prog.refractir_f0())
```

Vector-typed symbols must return a list with one element per lane.

### Name Mangling

RefractIR symbols may appear as global symbols (`@?name`) or local symbols (`%?name`). For translation targets,
**both are treated as external providers**. The naming scheme is stable and deterministic:

- Format: `<func>__<sym>`
- `<func>` is the function basename with sigils removed (e.g., `@f0` → `f0`)
- `<sym>` is the symbol basename with sigils removed (e.g., `@?c4` → `c4`, `%?k` → `k`)

Examples:
- `@f0` + `@?c4` → `f0__c4`
- `@f0` + `%?k`  → `f0__k`

Function names are mangled `refractir_<name>` in all targets (e.g.,
`@sum` → `refractir_sum`); `--emit-main` keeps `@main` unmangled.


## Python Target (v0.2.3)

Python has neither `goto` nor labeled `break`, so — unlike the C
backend (labels + `goto`) and the WASM backend (`loop` + `br_table`
PC-dispatch) — the Python backend **reconstructs genuine structured
control flow**: loops become `while` statements, branches become
`if`/`else`, and block labels survive as `# ^label` comments.

This is only possible when the CFG is **reducible** (every cycle has
a unique header block that dominates the whole cycle).
`--target python` therefore registers a reducibility check that
rejects irreducible functions with a static error naming the
offending branch; the same check is available on any target via
`--require-reducible`. The reconstruction pipeline — dominator tree,
loop nesting forest, control-tree builder, structured lowering — is
documented in [reducibility.md](./reducibility.md), together with the
`--dump-domtree` / `--dump-loops` / `--dump-control-tree` /
`--dump-lowered-tree` inspection flags.

### Runtime semantics

RefractIR's strict UB is checked **eagerly at runtime** by a small
helper preamble emitted once per module:

- `class RefractIRTrap(Exception)` — every UB event raises it, so an
  executed UB path exits nonzero with a traceback instead of
  continuing silently.
- Checked signed arithmetic `_iadd`/`_isub`/`_imul` — Python integers
  are unbounded, so overflow outside `iN` traps explicitly rather
  than wrapping.
- `_sdiv`/`_srem` — truncate toward zero and trap on division by zero
  and `INT_MIN % -1`; Python's flooring `//` and `%` are never used
  raw.
- Shifts trap on out-of-range amounts per spec §7.1.
- `f32` arithmetic round-trips each operation through
  `struct.pack/unpack('<f', …)` (RNE per IEEE 754); every FP result
  passes a finiteness check (`±∞`/NaN are UB).
- `i1` uses the spec's canonical `{0, -1}` values; vector `cmp`
  yields a `{0, -1}` mask list.

Aggregates and address-taken scalars lower to flat leaf-slot lists
with a provenance-tracked `_Ptr(buf, off, stride, lo, hi)` pointer
class: null/uninitialized/out-of-bounds dereference, cross-object
arithmetic, and cross-object relational comparison all trap. `undef`
slots hold a unique sentinel that traps on read. Non-addressable
scalars stay plain Python variables for readability.

Vectors compute as lane lists (comprehensions over `zip`), and
`--vec-lowering` selects the *storage form* of vector locals —
mirroring the C strategies: `array` (a plain lane list, the default),
`scalars` (`v_0 .. v_{N-1}` variables), `structarray` / `structscalars`
(per-shape helper classes `_vec_<N>_<elem>_arr` / `_scal`, emitted
once per used shape). Like their C twins, `scalars` and
`structscalars` reject dynamic lane indices at emission time; `vecext`
has no python analogue and is rejected. The chosen strategy is
stamped as a `# vec-lowering:` comment in the module header.

### Example

```sir
fun @sum(%n: i32) : i32 {
  sym %?step : value i32;
  let mut %i: i32 = 0;
  let mut %acc: i32 = 0;
^head:
  br %i < %n, ^body, ^done;
^body:
  %acc = %acc + %?step;
  %i = %i + 1;
  br ^head;
^done:
  ret %acc;
}
```

emits (after the preamble):

```python
def refractir_sum(n):
    i = 0
    acc = 0
    while (i) < (n):
        # ^body
        acc = _iadd(acc, sum__step(), 32)
        i = _iadd(i, 1, 32)
    return acc
```

Branches that leave several nested loops at once cannot use a native
Python statement; they lower to one-shot guard flags with cascaded
`break`s (`_brk_*`, `_cnt_*`, `_go_*`), hoisted to function entry.
Common shapes — single-level breaks, early `return` from any depth —
emit zero flags.


## Structured C (`--structured-lowering`, v0.2.3)

By default the C backend emits labels+`goto`, which accepts any CFG.
With `--structured-lowering` it reconstructs genuine structured
control flow through the same pipeline as the Python target —
`while (cond)`, `do { … } while (cond)`, `for (;;)`, `if`/`else`,
`bool` guard flags for multi-level exits, block labels as `// ^label`
comments — and therefore also **requires reducible control flow**
(the flag implies `--require-reducible`).

The mode changes only the function-body shape; expression emission,
types, intrinsics, vector lowering, symbols, `--split-by-source`, and
the sanitizer-based UB story are identical to the default goto
emission. One deliberate refinement: an executed `unreachable`
terminator traps (`__builtin_trap()`) instead of falling through.
Structured C participates fully in cross-validation (`make
cross-validation` runs both emission modes against the interpreter).

The flag is rejected on `--target wasm` (structured WASM is deferred)
and is a no-op on `--target python` (already structured). For the
same program as above, `--structured-lowering` emits:

```c
int32_t refractir_sum(int32_t refractir_n) {
  int32_t refractir_i = 0;
  int32_t refractir_acc = 0;
  while (((refractir_i)) < ((refractir_n))) {
    // ^body
    refractir_acc = ((refractir_acc) + (sum__step()));
    refractir_i = ((refractir_i) + (1));
  }
  return ((refractir_acc));
}
```


## Options

| Option             | Description                                |
| ------------------ | ------------------------------------------ |
| `--target c`       | Emit C source (default)                    |
| `--target wasm`    | Emit WebAssembly (WAT)                     |
| `--target python`  | Emit Python (requires reducible control flow) |
| `-o <file>`        | Output file (default: stdout)              |
| `-I <path>`        | Include path for resolving link-form `decl`s (may repeat) |
| `--require-reducible` | Reject functions with irreducible control flow (any target) |
| `--structured-lowering` | C target: emit `while`/`do-while`/`if` instead of labels+`goto` (implies `--require-reducible`) |
| `--vec-lowering <s>` | Vector lowering strategy: `vecext\|scalars\|array\|structscalars\|structarray` for C; python accepts all but `vecext` (no native SIMD value type); wasm only `array`. Default: `vecext` (C) / `array` (others) |
| `--split-by-source` | C target: emit one `<stem>.c` per source file plus `common.h` into the directory given by `-o` |
| `--emit-main`      | Do not mangle `@main` in emitted target code |
| `--no-module-tags` | Omit `(module ...)` tags in WASM output    |
| `--dump-ast`       | Dump the AST to stdout and exit            |
| `--dump-domtree`   | Dump per-function dominator trees and exit (see [reducibility.md](./reducibility.md)) |
| `--dump-loops`     | Dump per-function loop nesting forests and exit |
| `--dump-control-tree` | Dump per-function structured control trees and exit (implies `--require-reducible`) |
| `--dump-lowered-tree` | Dump control trees after structured lowering and exit (implies `--require-reducible`) |
| `-w`               | Inhibit all warning messages               |
| `--Werror`         | Make all warnings into errors              |
| `--no-require`     | Omit `require` checks from emitted code (useful for compiler testing) |
| `-h, --help`       | Print usage                                |


## Limitations (v0.2.3)

* `i1`, `i8`, `i16`, `i32`, `i64`, `f32`, `f64`, arrays, structs, and pointers (`ptr T`) lower to all three targets (C, WASM, Python).
* Function calls (`call`), link-form `decl` resolution (`-I`), and the standard intrinsics (§12) lower to all three targets, with one exception: the reify checksum intrinsics `@crc32_update` and `@check_chksum` lower to C and Python only — `symirc --target wasm` rejects a program that declares them (see `docs/intrinsics.md` §12.7).
* The Python target only accepts **reducible** control flow; irreducible functions are rejected with a static error (see [reducibility.md](./reducibility.md)). C and WASM accept arbitrary CFGs.
* Heap allocation is still out of scope; pointers always refer to stack-resident `let mut` locals (see spec §2.8).
* No optimization passes — the lowered C/WASM/Python follows the source closely.
* In WASM, pointers are 32-bit addresses into the linear memory; in C they are native C pointers; in Python they are provenance-tracked `_Ptr` objects over flat leaf-slot lists. Pointer arithmetic and `ptr - ptr` (element distance) are supported everywhere, but cross-object arithmetic remains UB per spec §7.5.
* WASM vector support currently unrolls operations lane-by-lane on the shadow stack (as aggregates). Native WebAssembly SIMD-128 lowering (using the `v128` type and instructions) is a non-goal for v0.2.2, planned for a later version (SPEC §13). Python computes vectors as lane lists; `--vec-lowering` varies the storage form of vector locals (all C strategies except `vecext`).
* The Python target does not yet participate in cross-validation (`make cross-validation`) — bit-exact `Result:` line parity (`printf %a` vs `float.hex()`) is a planned follow-up. It is covered by `make test-backends` (exit-code semantics over the compile corpus).

## Refinement and Undefined Behavior Semantics

The compilers perform **semantic refinement** over the input RefractIR program. Under refinement:
- The compiled target program (C, WebAssembly, or Python) must not exhibit any observable behavior that was not allowed by the original source program.
- This semantic equivalence is guaranteed only when the input program is **UB-free** (free of Undefined Behavior).
- If the input program executes a path containing Undefined Behavior (such as signed integer overflow, division/modulo by zero, or invalid pointer navigation/comparison), the behavior of the target program is **not guaranteed** and may deviate from strict RefractIR interpreter/solver checks (which model UB as a fatal execution constraint).
- **Per-target UB strictness**:
  - The **Python target** is the strictest: the emitted helper preamble checks every UB-capable operation eagerly at runtime and raises `RefractIRTrap` (nonzero exit) the moment UB is executed — signed overflow, division/remainder by zero, shift-range violations, non-finite FP results, out-of-bounds indices and lane accesses, `undef` reads, and null/out-of-bounds/cross-object pointer operations are all trapped without any external tooling.
  - For the **C target**, we try our best to preserve the trapping semantics of RefractIR undefined behaviors. Because many of RefractIR's undefined behaviors map cleanly to native C undefined behaviors, compiling the output C code with GCC and enabling sanitizers (e.g., `-fsanitize=address,undefined,float-cast-overflow,pointer-compare,pointer-subtract`) allows the runtime to catch and trap these events.
  - However, **this effort is not put on the WebAssembly (WASM) target**. The WASM backend lowers RefractIR constructs to clean, native WASM instructions without inserting safety checks or runtime sanitizer assertions. Any executed undefined behavior on WASM will follow standard WASM instruction behavior (e.g. wrapping on signed overflow, returning 0 on modulo overflow, or ignoring relational pointer provenance).

### Minimal WebAssembly Example (Signed Modulo Overflow)

Consider the following RefractIR program:

```sir
fun @main() : i32 {
  let mut %min: i32 = -2147483648; // INT_MIN
  let mut %neg1: i32 = -1;
  let mut %res: i32 = 0;
^entry:
  // INT_MIN % -1 triggers signed overflow (Undefined Behavior under Spec §7.1 Rule 4)
  %res = %min % %neg1;
  ret %res;
}
```

- **RefractIR Semantics**: Since this operation triggers signed overflow, a strict RefractIR symbolic execution path or interpreter execution will trap/fail immediately, treating the execution as invalid.
- **WebAssembly compilation**: When translated to WebAssembly, the modulo instruction is compiled directly to WASM's native signed remainder instruction:
  ```wat
  local.get $min
  local.get $neg1
  i32.rem_s
  ```
  In the WebAssembly specification, `i32.rem_s` with `INT_MIN` and `-1` does not trap (unlike `i32.div_s`); instead, it returns `0` silently. Consequently, the compiled target program exits successfully with `0`, demonstrating that the behavior of target code is not guaranteed once undefined behavior is introduced in the source.
