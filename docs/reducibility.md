# CFG Reducibility & Structured Control Flow

RefractIR is a goto-style CFG IR: a function body is a set of basic
blocks connected by explicit `br` terminators, with no structural
restrictions. That is ideal for symbolic execution and SMT constraint
generation, but some translation targets cannot express arbitrary
control flow — Python has neither `goto` nor labeled `break`. Rather
than degrade those targets to an unreadable dispatch-loop encoding,
`symirc` restricts them to **reducible** CFGs and reconstructs genuine
structured control flow (`while` / `if`) from the dominator tree.

This document specifies the analyses and the transform that make this
possible. They live in `include/analysis` + `src/analysis` and are
plain build-from-CFG structs, reusable outside the pass pipeline; only
the reducibility check has a diagnostic pass surface.

| Stage | Files | Dump flag |
|---|---|---|
| Dominator tree | `dominators.{hpp,cpp}` | `--dump-domtree` |
| Reducibility check | `reducibility.{hpp,cpp}` | (diagnostics; `--require-reducible`) |
| Loop nesting forest | `loop_info.{hpp,cpp}` | `--dump-loops` |
| Control-tree builder | `structurizer.{hpp,cpp}` | `--dump-control-tree` |
| Structured lowering | `structured_lowering.{hpp,cpp}` | `--dump-lowered-tree` |

All five stages run inside `symirc`. The Python backend — and the C
backend under `--structured-lowering` — consume the *lowered* control
tree (§5) at emit time. The WASM backend under `--structured-lowering`
consumes the *unlowered* tree directly: its native multi-level `br`
expresses every transfer, so no lowering is needed (§4). See
[symirc.md](./symirc.md).


## 1. Reducibility

**Definition.** Fix a depth-first ordering of the CFG and let
`rpo(b)` be a block's reverse-postorder number. An edge `u → v` is
*retreating* iff `rpo(v) <= rpo(u)`. A retreating edge is a *back
edge* iff its target dominates its source. A CFG is **reducible** iff
every retreating edge is a back edge.

Equivalently: every cycle has a unique entry block (its *header*)
that dominates every block in the cycle. Irreducible control flow —
a branch that enters a loop "from the side", past its header —
cannot be expressed with `while`/`break`/`continue` without
duplicating code, which is why structuring targets reject it.

This dominance-based test is chosen over the classic T1/T2
interval-collapse test because the dominator tree is needed anyway
(for loops and structuring) and because it names the exact offending
edge for diagnostics:

```text
  10 |   br %x < 10, ^a, ^exit;
     |   ^
     |   error: Irreducible control flow: branch from ^b to ^a re-enters a loop whose header does not dominate ^b
   5 | ^a:
     | ^
     | note: ^a is reached both from above and by this retreating edge, so it is not a unique loop header
```

The check runs as a pass (`ReducibilityCheck`) registered by the
driver when the target cannot express irreducible control flow
(`--target python`), when a control-tree dump is requested, or
unconditionally via `symirc --require-reducible`. Rejection is a
static error (exit code 4), reported before any code is emitted.

Note that reducibility is a property of the CFG *shape*, not of the
program's behavior: the same loop written with a single header block
is accepted. `rysmith`/`rylink` only generate reducible control flow,
so the reify pipeline is unaffected by the restriction.


## 2. Dominator tree (`DomTree`)

Built with the Cooper–Harvey–Kennedy iterative algorithm: immediate
dominators are intersected over the reverse postorder until a
fixpoint. Near-linear on the small CFGs RefractIR produces, with no
auxiliary forests.

- `idom[b]` — immediate dominator per block (`idom[entry] == entry`).
- `children[b]` — dominator-tree children, ordered by RPO number so
  traversals are deterministic.
- `rpoNumber[b]` — the block's reverse-postorder position; doubles as
  the retreating-edge classifier.
- `dominates(a, b)` — reflexive dominance query via idom-chain walk.

Blocks unreachable from the entry hold the sentinel `kNone` in both
`idom` and `rpoNumber` and are excluded from everything downstream
(they are separately diagnosed by the reachability analysis).

`--dump-domtree` prints one section per function in a stable
label-based format:

```text
domtree @sum:
  ^head: (root)
  ^body: idom=^head
  ^done: idom=^head
```


## 3. Loop nesting forest (`LoopInfo`)

Natural loops are discovered from back edges. All back edges
targeting the same header are merged into a single `Loop` — that
merge is the *only* normalization structured emission needs:

- the **header** is unique by construction on a reducible CFG;
- extra **latches** (back-edge sources) are just extra `continue`
  sites;
- each **exit edge** (source in loop, destination outside)
  independently lowers to a `break`.

No preheaders or dedicated-exit blocks are synthesized and the block
list is never mutated. Loops nest into a forest (`parent`,
`children`, `depth`), with `innermostLoop[b]` mapping each block to
its innermost containing loop.

On an irreducible CFG, `LoopInfo` silently ignores the irreducible
cycles (only true back edges form loops) — run `ReducibilityCheck`
first when that matters.

`--dump-loops` prints, per function:

```text
loops @sum:
  loop 0: header=^head depth=1 parent=none
    latches: ^body
    blocks: ^head ^body
    exits: ^head->^done
```


## 4. Control-tree builder (`Structurizer`)

Reconstructs a structured control tree from a reducible CFG using the
dominator-tree translation of Ramsey, *"Beyond Relooper"* (ICFP
2022). The method is **total** on reducible CFGs — no node splitting,
no CFG mutation — and the tree's nesting equals the emitted
statement nesting:

- A block's dominator-tree children that are *merge nodes* (≥ 2
  forward in-edges) or loop headers become an RPO-ordered *follower
  sequence* after the block's own content.
- Children with a single forward predecessor inline into the branch
  arm that reaches them.
- A loop header wraps its natural-loop body in a `Loop` node; its
  dominated out-of-loop blocks follow the loop.
- Each CFG edge then classifies against the context stack as inline,
  fall-through, `continue` (back edge), `break` of one or more levels
  (loop exit), or a forward jump to a non-immediate follower.
  Reducibility plus dominance guarantee every edge is resolvable.

The resulting `ControlTree` is **target-neutral**: transfer nodes say
*what* must happen, not how a backend spells it.

| Node | Meaning |
|---|---|
| `Seq` | Ordered children at one nesting level |
| `BlockStmts` | One block's straight-line instructions (terminator excluded) |
| `If` | A conditional terminator with then/else subtrees |
| `Loop` | A natural loop (`while True` until a `Break` leaves it) |
| `Break{target, levels}` | Leave `levels ≥ 1` enclosing loops, resume at a pending join |
| `Continue{header, levels}` | Back edge, after first leaving `levels ≥ 0` inner loops |
| `FallThrough` | Fall through to the next pending join |
| `JumpJoin{target, levels}` | Forward jump that must skip intermediate join subtrees |
| `Return` / `Trap` | A `ret` / `unreachable` terminator |

A target with labeled break (or WASM's `br N`) lowers
`Break{levels=2}` natively — the WASM backend does exactly this,
emitting a named `br` to the target's `block`/`loop` scope, so it
consumes this tree unchanged; targets without one run structured
lowering first (§5).

`--dump-control-tree` (implies `--require-reducible`) prints an
indented rendering:

```text
control-tree @sum:
  loop 0 header=^head
    block ^head
    if ^head
      then:
        block ^body
        continue ^head levels=0
      else:
        break ^done levels=1
  block ^done
  return ^done
```


## 5. Structured lowering (`StructuredLowering`)

Rewrites a control tree for targets with only single-level `break` /
`continue` and no `goto` (the Python backend, and the C backend under
`--structured-lowering`). Multi-level transfers become **one-shot
guard flags** plus cascaded single-level breaks — pay-as-you-go:
common shapes emit zero flags.

- `Break{levels=L>1}` → set a flag + `break`, then an
  `if <flag>: break` cascade after each of the `L-1` enclosing loops
  (the final cascade resets the flag).
- `Continue{levels=k>0}` → the same cascade, ending in
  `if <flag>: <flag> = False; continue` inside the target loop.
- `JumpJoin` → set a flag (+ break/cascades when loops are crossed);
  the skipped join subtrees are wrapped in `if not <flag>:` guards
  and the flag resets where the target subtree is reached.
- `FallThrough` nodes and tail-position `continue levels=0` are
  dropped; emptied `If` arms are removed (negating the condition when
  only the then-arm emptied).
- `while True` loops whose header has no instructions and whose
  header `If` has a single-level-break arm peephole into a
  *condition loop* (`while cond:` / `while not cond:`).
- `while True` loops whose body *ends* with a single-level
  `if cond: break` peephole into a `DoWhile` node
  (`do { body } while (cond);` in structured C) — but only when the
  rest of the body has no continue site binding to the loop, because
  C's `continue` inside do-while evaluates the condition instead of
  unconditionally re-entering the body. Targets without do-while
  (Python) re-expand the node to the exact pre-peephole form.
- Header-test loops whose header carries instructions *rotate*
  (classic loop inversion): `loop { H; if cond: R else break }`
  becomes `H; while cond: { R; H }`, duplicating H's statements once
  so the loop condition is visible instead of an infinite loop with
  a break. Same continue-site veto as do-while: rotated, a `continue`
  in R would skip the trailing H and re-test the condition early.
  Loops with scattered mid-body exits or live continue sites remain
  `while True` — no single-condition form expresses them without
  deeper restructuring.

The lowered tree contains no `FallThrough`/`JumpJoin`, every `Break`
has `levels == 1`, and every `Continue` has `levels == 0` — only
constructs a single-level-break language expresses directly. Flags
are `False` except between their set and final cascade/reset, so
re-entering the region is safe; emitters declare and initialize them
at function entry.

`ret` needs no flags at all: Python and C allow `return` anywhere,
so `Return` nodes are emitted in place.

`--dump-lowered-tree` (implies `--require-reducible`) shows the tree
the Python backend actually prints:

```text
lowered-tree @sum:
  while 0 header=^head
    block ^body
  block ^done
  return ^done
```


## 6. Worked example

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

The dumps in §§2–5 are this function's. `symirc --target python`
emits (after the semantics preamble):

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

The loop header's `br` became the `while` condition (CondLoop
peephole), the back edge disappeared into the loop structure, block
labels survive as comments, and no guard flags were needed.


## 7. CLI surface

| Flag | Effect |
|---|---|
| `--require-reducible` | Reject irreducible functions (any target) |
| `--structured-lowering` | C target: emit while/do-while/if from the lowered control tree instead of labels+goto. WASM target: emit `block`/`loop`/`if` with named `br` labels from the unlowered tree instead of the `$__pc` dispatch loop. Implies `--require-reducible` |
| `--dump-domtree` | Print per-function dominator trees and exit |
| `--dump-loops` | Print per-function loop forests and exit |
| `--dump-control-tree` | Print structured control trees and exit (implies `--require-reducible`) |
| `--dump-lowered-tree` | Print control trees after structured lowering and exit (implies `--require-reducible`) |

`--target python` and `--structured-lowering` register the
reducibility check automatically. Irreducible input fails with exit
code 4 (static error). `--structured-lowering` is a no-op on python
(already structured); on the C and WASM targets it reconstructs
structured control flow (the WASM default remains the dispatch loop,
which also handles irreducible CFGs).


## 8. Testing

Fixtures live in `test/reducibility/` and are run by
`test/lib/run_reducibility_tests.py` (part of `make test-frontend`):

```bash
python3 -m test.lib.run_reducibility_tests test/reducibility ./symirc
```

Each `.sir` fixture passes its dump flag through
`// COMPILER_ARGS:`; a sibling `<name>.sir.expected` file pins the
dump output byte-for-byte, and `// EXPECT: FAIL:StaticError` fixtures
pin the irreducibility diagnostics.
