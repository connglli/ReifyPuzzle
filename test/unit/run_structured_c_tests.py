"""End-to-end tests for `symirc --structured-lowering` on the C target:

  (A) Structured emission reconstructs genuine `while`/`if` control
      flow: the output contains no `goto` and no block labels.
  (B) `--structured-lowering` implies `--require-reducible`:
      irreducible input fails with the static-error exit code (4).
  (C) The WASM target rejects the flag (structured WASM is deferred);
      the Python target accepts it as a no-op (already structured).
  (D) The emitted structured C compiles with gcc and computes the same
      result as the default goto emission.
  (E) Multi-level loop exits lower to one-shot guard flags with
      cascaded breaks, and still compute the right result.

Run as:

  python3 -m test.unit.run_structured_c_tests ./symirc

Each test prints PASS or FAIL; exit code reflects the worst result.
"""

import os
import re
import subprocess
import sys
import tempfile

GREEN = "\033[32m"
RED = "\033[31m"
GRAY = "\033[90m"
NC = "\033[0m"

results = []  # (name, passed, detail)


def run(cmd, **kw):
  print(f"  {GRAY}[RUN>]{NC} " + " ".join(cmd))
  return subprocess.run(cmd, capture_output=True, text=True, timeout=60, **kw)


def check(name, ok, detail=""):
  results.append((name, ok, detail))
  color = GREEN if ok else RED
  tag = "PASS" if ok else "FAIL"
  print(f"  [{color}{tag}{NC}] {name}" + (f" — {detail}" if detail and not ok else ""))


def write_sir(src):
  with tempfile.NamedTemporaryFile("w", suffix=".sir", delete=False) as f:
    f.write(src)
    return f.name


def gcc_run(c_code, workdir):
  """Compile C source with gcc and run it; returns the process exit code."""
  c_path = os.path.join(workdir, "prog.c")
  bin_path = os.path.join(workdir, "prog")
  with open(c_path, "w") as f:
    f.write(c_code)
  r = subprocess.run(
    ["gcc", "-std=c11", "-O0", "-o", bin_path, c_path, "-lm"],
    capture_output=True,
    text=True,
    timeout=60,
  )
  if r.returncode != 0:
    return None, f"gcc failed: {r.stderr[:400]}"
  r = subprocess.run([bin_path], capture_output=True, text=True, timeout=30)
  return r.returncode, ""


# Reducible: a single while-shaped loop summing 0..9 -> 45.
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

# Irreducible: the cycle a <-> b has two entries (entry -> a and entry -> b
# ... via the conditional), so ^a is not a unique loop header.
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

# Nested loops where ^found (inside both) branches to ^done (after the
# outer loop): a two-level break that needs a guard-flag cascade.
# First hit of i*j == 6 is i=2, j=3 -> returns 23.
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


# Latch-branch loop: the body always runs once -> do-while form.
# Sums 0..9 -> 45.
DO_WHILE = """fun @main() : i32 {
  let mut %i: i32 = 0;
  let mut %acc: i32 = 0;
^body:
  %acc = %acc + %i;
  %i = %i + 1;
  br %i < 10, ^body, ^done;
^done:
  ret %acc;
}
"""

# do-while-shaped outer loop whose body contains a flagcontinue (from a
# two-level continue): C `continue` inside do-while evaluates the
# condition, so the peephole must NOT fire. Interpreter result: 16.
DO_WHILE_EXCLUDED = """fun @main() : i32 {
  let mut %i: i32 = 0;
  let mut %j: i32 = 0;
  let mut %acc: i32 = 0;
^entry:
  br ^oh;
^oh:
  %acc = %acc + 1;
  br %acc < 5, ^ihead, ^skip;
^ihead:
  %j = 0;
  br ^ih;
^ih:
  br %j < 3, ^ib, ^m;
^ib:
  %j = %j + 1;
  br %j == 2, ^oh, ^ih;
^skip:
  %acc = %acc + 2;
  br %acc < 50, ^m, ^done;
^m:
  %i = %i + 1;
  br %i < 4, ^oh, ^done;
^done:
  ret %acc;
}
"""


def test_structured_shape(symirc):
  """(A) --structured-lowering emits while/if, no goto, no labels."""
  path = write_sir(LOOP_SUM)
  try:
    r = run([symirc, path, "--structured-lowering", "--emit-main"])
    ok = r.returncode == 0
    detail = f"rc={r.returncode}, stderr={r.stderr[:200]!r}"
    if ok:
      no_goto = "goto" not in r.stdout
      no_label = not re.search(r"^\s*refractir_\w+: ;$", r.stdout, re.M)
      has_while = "while (" in r.stdout
      ok = no_goto and no_label and has_while
      detail = f"goto-free={no_goto}, label-free={no_label}, has-while={has_while}"
    check("structured C: while loop, no goto, no labels", ok, detail)
  finally:
    os.unlink(path)


def test_implies_require_reducible(symirc):
  """(B) irreducible input + --structured-lowering -> StaticError (4)."""
  path = write_sir(IRREDUCIBLE)
  try:
    r = run([symirc, path, "--structured-lowering"])
    ok = r.returncode == 4 and "rreducible" in r.stderr
    check(
      "structured C: irreducible input rejected with StaticError",
      ok,
      f"rc={r.returncode}, stderr={r.stderr[:200]!r}",
    )
  finally:
    os.unlink(path)


def test_target_gating(symirc):
  """(C) wasm rejects --structured-lowering; python accepts it as no-op."""
  path = write_sir(LOOP_SUM)
  try:
    r = run([symirc, path, "--structured-lowering", "--target", "wasm"])
    wasm_ok = r.returncode != 0 and "structured-lowering" in r.stderr
    r2 = run([symirc, path, "--structured-lowering", "--target", "python"])
    py_ok = r2.returncode == 0 and "def " in r2.stdout
    check(
      "structured C: wasm rejects flag, python no-ops",
      wasm_ok and py_ok,
      f"wasm rc={r.returncode} stderr={r.stderr[:150]!r}; python rc={r2.returncode}",
    )
  finally:
    os.unlink(path)


def test_compiles_and_runs(symirc):
  """(D) structured output compiles and matches the goto emission's result."""
  path = write_sir(LOOP_SUM)
  try:
    with tempfile.TemporaryDirectory() as d:
      r = run([symirc, path, "--structured-lowering", "--emit-main"])
      if r.returncode != 0:
        check(
          "structured C: compiles and runs (sum 0..9 = 45)",
          False,
          f"symirc rc={r.returncode}",
        )
        return
      rc, err = gcc_run(r.stdout, d)
      ok = rc == 45
      check("structured C: compiles and runs (sum 0..9 = 45)", ok, err or f"exit={rc}")
  finally:
    os.unlink(path)


def test_multilevel_break_flags(symirc):
  """(E) two-level break lowers to guard flags and computes 23."""
  path = write_sir(MULTILEVEL_BREAK)
  try:
    with tempfile.TemporaryDirectory() as d:
      r = run([symirc, path, "--structured-lowering", "--emit-main"])
      if r.returncode != 0:
        check(
          "structured C: multi-level break via flags",
          False,
          f"symirc rc={r.returncode}",
        )
        return
      has_flag = "_brk_" in r.stdout
      no_goto = "goto" not in r.stdout
      rc, err = gcc_run(r.stdout, d)
      ok = has_flag and no_goto and rc == 23
      check(
        "structured C: multi-level break via flags",
        ok,
        err or f"flag={has_flag}, goto-free={no_goto}, exit={rc}",
      )
  finally:
    os.unlink(path)


def test_split_by_source_structured(symirc):
  """--split-by-source honors --structured-lowering in every emitted .c."""
  path = write_sir(LOOP_SUM)
  try:
    with tempfile.TemporaryDirectory() as d:
      r = run(
        [
          symirc,
          path,
          "--structured-lowering",
          "--split-by-source",
          "-o",
          d,
        ]
      )
      if r.returncode != 0:
        check("structured C: split-by-source emission", False, f"rc={r.returncode}")
        return
      cs = [f for f in os.listdir(d) if f.endswith(".c")]
      with_goto = [f for f in cs if "goto" in open(os.path.join(d, f)).read()]
      check(
        "structured C: split-by-source is goto-free",
        bool(cs) and not with_goto,
        f"goto in: {with_goto}" if cs else "no .c emitted",
      )
  finally:
    os.unlink(path)


def test_do_while(symirc):
  """(F) latch-branch loop emits `do { ... } while (...)` and computes 45."""
  path = write_sir(DO_WHILE)
  try:
    with tempfile.TemporaryDirectory() as d:
      r = run([symirc, path, "--structured-lowering", "--emit-main"])
      if r.returncode != 0:
        check("structured C: do-while emission", False, f"symirc rc={r.returncode}")
        return
      has_do = "do {" in r.stdout and "} while (" in r.stdout
      rc, err = gcc_run(r.stdout, d)
      ok = has_do and rc == 45
      check(
        "structured C: do-while emission", ok, err or f"do-while={has_do}, exit={rc}"
      )
  finally:
    os.unlink(path)


def test_do_while_excluded_on_continue(symirc):
  """(G) a continue site in the body vetoes do-while; result matches goto (16)."""
  path = write_sir(DO_WHILE_EXCLUDED)
  try:
    with tempfile.TemporaryDirectory() as d:
      rs = run([symirc, path, "--structured-lowering", "--emit-main"])
      rg = run([symirc, path, "--emit-main"])
      if rs.returncode != 0 or rg.returncode != 0:
        check(
          "structured C: continue site vetoes do-while",
          False,
          f"symirc rc structured={rs.returncode} goto={rg.returncode}",
        )
        return
      no_do = "do {" not in rs.stdout
      rc_s, err_s = gcc_run(rs.stdout, d)
      rc_g, err_g = gcc_run(rg.stdout, d)
      ok = no_do and rc_s == 16 and rc_g == 16
      check(
        "structured C: continue site vetoes do-while",
        ok,
        err_s or err_g or f"no-do={no_do}, structured={rc_s}, goto={rc_g}",
      )
  finally:
    os.unlink(path)


def main():
  if len(sys.argv) != 2:
    print("usage: python3 -m test.unit.run_structured_c_tests <symirc>")
    return 2
  symirc = sys.argv[1]

  print("== structured C backend tests ==")
  test_structured_shape(symirc)
  test_implies_require_reducible(symirc)
  test_target_gating(symirc)
  test_compiles_and_runs(symirc)
  test_multilevel_break_flags(symirc)
  test_split_by_source_structured(symirc)
  test_do_while(symirc)
  test_do_while_excluded_on_continue(symirc)

  failed = [name for name, ok, _ in results if not ok]
  print(
    f"\nSummary (structured_c_tests): {len(results) - len(failed)}/{len(results)} passed."
  )
  return 1 if failed else 0


if __name__ == "__main__":
  sys.exit(main())
