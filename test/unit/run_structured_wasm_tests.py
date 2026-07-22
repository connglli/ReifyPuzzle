"""End-to-end tests for `symirc --structured-lowering` on the WASM target:

  (A) Structured emission reconstructs genuine block/loop/if control
      flow: the output has no $__pc dispatch loop and no br_table.
  (B) `--structured-lowering` implies `--require-reducible`:
      irreducible input fails with the static-error exit code (4).
  (C) The emitted structured WASM validates and computes the same
      result as the default dispatch-loop emission.
  (D) Multi-level loop exits lower to native multi-level `br` (WASM's
      br N, spelled as a named label) — no guard flags — and still
      compute the right result.
  (E) A forward jump that skips an intermediate merge (jumpjoin) and a
      plain merge (join block) both run correctly.

Runs the generated WAT under `wasmtime`; SKIPs gracefully when no WASM
runtime is installed.

Run as:

  python3 -m test.unit.run_structured_wasm_tests ./symirc

Each test prints PASS/FAIL/SKIP; exit code reflects the worst result.
"""

import os
import shutil
import subprocess
import sys
import tempfile

GREEN = "\033[32m"
RED = "\033[31m"
YELLOW = "\033[33m"
GRAY = "\033[90m"
NC = "\033[0m"

WASMTIME = shutil.which("wasmtime")

results = []  # (name, status)  status in {"PASS","FAIL","SKIP"}


def run(cmd, **kw):
  print(f"  {GRAY}[RUN>]{NC} " + " ".join(cmd))
  return subprocess.run(cmd, capture_output=True, text=True, timeout=60, **kw)


def check(name, ok, detail=""):
  status = "PASS" if ok else "FAIL"
  results.append((name, status))
  color = GREEN if ok else RED
  print(
    f"  [{color}{status}{NC}] {name}" + (f" — {detail}" if detail and not ok else "")
  )


def skip(name, detail=""):
  results.append((name, "SKIP"))
  print(f"  [{YELLOW}SKIP{NC}] {name}" + (f" — {detail}" if detail else ""))


def write_sir(src):
  with tempfile.NamedTemporaryFile("w", suffix=".sir", delete=False) as f:
    f.write(src)
    return f.name


def wat_run(symirc, sir_path, structured, workdir):
  """Emit WAT (structured or dispatch), then invoke `main` under wasmtime.

  Returns (value:str|None, returncode:int, detail:str). `value` is the
  wasmtime-printed result, or None on a compile/validation error."""
  wat = os.path.join(workdir, ("s" if structured else "d") + ".wat")
  args = [symirc, sir_path, "--target", "wasm", "--emit-main", "-o", wat]
  if structured:
    args.insert(-2, "--structured-lowering")
  r = run(args)
  if r.returncode != 0:
    return None, r.returncode, f"symirc rc={r.returncode}: {r.stderr[:200]}"
  r = subprocess.run(
    ["wasmtime", "run", "--invoke", "main", wat],
    capture_output=True,
    text=True,
    timeout=30,
  )
  if r.returncode != 0:
    return None, r.returncode, f"wasmtime rc={r.returncode}: {r.stderr[-200:]}"
  return r.stdout.strip(), 0, ""


# ---- fixtures (shared shapes with the structured-C suite) -----------------

LOOP_SUM = """fun @main() : i32 {
  let mut %i: i32 = 0;
  let mut %acc: i32 = 0;
^head:
  br %i < 10, ^body, ^done;
^body:
  %acc = %acc + %i;
  %i = %i + 1;
  br ^head;
^done:
  ret %acc;
}
"""

IRREDUCIBLE = """fun @main() : i32 {
  let mut %x: i32 = 0;
^entry:
  br %x < 5, ^a, ^b;
^a:
  %x = %x + 1;
  br ^b;
^b:
  %x = %x + 2;
  br %x < 10, ^a, ^exit;
^exit:
  ret %x;
}
"""

# Nested loops; the inner ^found breaks to ^done past the outer loop: a
# two-level exit. First i*j == 6 is i=2, j=3 -> 23.
MULTILEVEL_BREAK = """fun @main() : i32 {
  let mut %i: i32 = 0;
  let mut %j: i32 = 0;
  let mut %r: i32 = 0;
^outer:
  br %i < 4, ^iinit, ^done;
^iinit:
  %j = 0;
  br ^inner;
^inner:
  br %j < 4, ^chk, ^olatch;
^chk:
  br %i * %j == 6, ^found, ^jstep;
^jstep:
  %j = %j + 1;
  br ^inner;
^olatch:
  %i = %i + 1;
  br ^outer;
^found:
  %r = 10 * %i + %j;
  br ^done;
^done:
  ret %r;
}
"""

# ^entry dominates two merge joins ^f1, ^f2; the edge ^A -> ^f2 skips the
# pending ^f1 subtree (a jumpjoin). a=-1,b=10 exercises that edge: A taken
# (a<0), then b<5 false -> jump straight to f2, x stays 0.
JUMPJOIN = """fun @main() : i32 {
  let mut %a: i32 = -1;
  let mut %b: i32 = 10;
  let mut %x: i32 = 0;
^entry:
  br %a < 0, ^A, ^B;
^A:
  br %b < 5, ^f1, ^f2;
^B:
  %x = 1;
  br ^f1;
^f1:
  %x = %x + 10;
  br ^f2;
^f2:
  ret %x;
}
"""

# `ptrindex` + `load` reached at a block that falls through (not via a
# `br`): its bounds-check must be stack-neutral or WASM validation trips
# on "values remaining on stack". a[1] == 9 -> 0.
PTR_INDEX = """fun @main() : i32 {
  let mut %a: [2] i32 = { 5, 9 };
  let mut %pa: ptr [2] i32 = null;
  let mut %ep: ptr i32 = null;
  let mut %g: i32 = 0;
^entry:
  %pa = addr %a;
  %ep = ptrindex %pa, 1;
  %g = load %ep;
  br %g == 9, ^ok, ^bad;
^ok:
  ret 0;
^bad:
  ret 1;
}
"""

# Plain merge with no loop: |x| via a join block. x=-7 -> 7.
DIAMOND = """fun @main() : i32 {
  let mut %x: i32 = -7;
  let mut %r: i32 = 0;
^entry:
  br %x < 0, ^neg, ^pos;
^neg:
  %r = 0 - %x;
  br ^join;
^pos:
  %r = %x;
  br ^join;
^join:
  ret %r;
}
"""


def test_structured_shape(symirc):
  """(A) --structured-lowering emits block/loop/if, no dispatch loop."""
  path = write_sir(LOOP_SUM)
  try:
    r = run([symirc, path, "--structured-lowering", "--target", "wasm", "--emit-main"])
    ok = r.returncode == 0
    detail = f"rc={r.returncode}, stderr={r.stderr[:200]!r}"
    if ok:
      has_loop = "(loop $__cont" in r.stdout
      no_dispatch = "dispatch_loop" not in r.stdout and "br_table" not in r.stdout
      ok = has_loop and no_dispatch
      detail = f"has-loop={has_loop}, dispatch-free={no_dispatch}"
    check("structured wasm: block/loop/if, no dispatch loop", ok, detail)
  finally:
    os.unlink(path)


def test_implies_require_reducible(symirc):
  """(B) irreducible input + --structured-lowering -> StaticError (4)."""
  path = write_sir(IRREDUCIBLE)
  try:
    r = run([symirc, path, "--structured-lowering", "--target", "wasm"])
    ok = r.returncode == 4 and "rreducible" in r.stderr
    check(
      "structured wasm: irreducible input rejected with StaticError",
      ok,
      f"rc={r.returncode}, stderr={r.stderr[:200]!r}",
    )
  finally:
    os.unlink(path)


def _run_case(symirc, name, src, expected, needs=()):
  """Compile+run under both emitters; both must equal `expected`, and the
  structured output must contain every substring in `needs`."""
  if not WASMTIME:
    skip(name, "no wasmtime")
    return
  path = write_sir(src)
  try:
    with tempfile.TemporaryDirectory() as d:
      sval, _, sdetail = wat_run(symirc, path, True, d)
      dval, _, ddetail = wat_run(symirc, path, False, d)
      if sval is None:
        check(name, False, f"structured: {sdetail}")
        return
      if dval is None:
        check(name, False, f"dispatch: {ddetail}")
        return
      # Substring checks look at the structured WAT emitted above.
      wat = open(os.path.join(d, "s.wat")).read()
      missing = [s for s in needs if s not in wat]
      ok = sval == str(expected) and dval == str(expected) and not missing
      check(
        name,
        ok,
        f"structured={sval}, dispatch={dval}, expected={expected}, missing={missing}",
      )
  finally:
    os.unlink(path)


def main():
  if len(sys.argv) != 2:
    print("usage: python3 -m test.unit.run_structured_wasm_tests <symirc>")
    return 2
  symirc = sys.argv[1]

  print("== structured WASM backend tests ==")
  test_structured_shape(symirc)
  test_implies_require_reducible(symirc)
  if not WASMTIME:
    skip("structured wasm: runtime cases", "wasmtime not installed")
  else:
    # (C) simple loop; (D) two-level break via a named multi-level br;
    # (E) jumpjoin + plain merge.
    _run_case(symirc, "structured wasm: loop sum 0..9 = 45", LOOP_SUM, 45)
    _run_case(
      symirc,
      "structured wasm: two-level break via br label = 23",
      MULTILEVEL_BREAK,
      23,
      needs=("br $__jn",),
    )
    _run_case(symirc, "structured wasm: jumpjoin = 0", JUMPJOIN, 0, needs=("br $__jn",))
    _run_case(symirc, "structured wasm: diamond merge |−7| = 7", DIAMOND, 7)
    _run_case(
      symirc, "structured wasm: ptrindex bounds check stack-neutral", PTR_INDEX, 0
    )

  failed = [n for n, s in results if s == "FAIL"]
  passed = [n for n, s in results if s == "PASS"]
  print(
    f"\nSummary (structured_wasm_tests): {len(passed)} passed, "
    f"{len(failed)} failed, {len(results) - len(passed) - len(failed)} skipped."
  )
  return 1 if failed else 0


if __name__ == "__main__":
  sys.exit(main())
