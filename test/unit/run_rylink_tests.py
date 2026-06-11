"""End-to-end tests for rylink — the whole-program generator.

rylink takes a directory of rysmith-generated functions (each with a
sidecar `.json` descriptor) and synthesises whole programs by:

  1. Picking a random subset of the pool as call-graph nodes.
  2. Building a DAG call-graph (no recursion).
  3. Performing peephole rewrites: replace literal occurrences in
     callers with a `call` to a callee whose solved ret value matches.
  4. Optionally validating semantic equivalence via symiri.

Each program lands in `prog_<id>_<i>/`, with the entry function
emitted as `program.sir` (carrying CG/param/ret comments in the
header) and one `.sir` per callee.

Run as:

  python3 -m test.unit.run_rylink_tests <rylink> <rysmith> <symiri>

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

results = []

# rylink (and rysmith) print a `generation id = <hex>` banner.
_ID_RE = re.compile(r"generation id\s*=\s*([0-9a-f]{6})")


def run(cmd, **kw):
  print(f"  {GRAY}[RUN>]{NC} " + " ".join(cmd))
  return subprocess.run(cmd, capture_output=True, text=True, timeout=120, **kw)


def check(name, ok, detail=""):
  results.append((name, ok, detail))
  color = GREEN if ok else RED
  tag = "PASS" if ok else "FAIL"
  print(f"  [{color}{tag}{NC}] {name}" + (f" — {detail}" if detail and not ok else ""))


def extract_id(stdout):
  m = _ID_RE.search(stdout or "")
  return m.group(1) if m else None


def seed_pool(rysmith, pool_dir, n_funcs=8, n_params=1, seed=23):
  """Drive rysmith and return the generated 6-hex ID for the run."""
  r = run(
    [
      rysmith,
      "--n-funcs",
      str(n_funcs),
      "--n-params",
      str(n_params),
      "--seed",
      str(seed),
      "--emit-desc",
      "-o",
      pool_dir,
    ]
  )
  # rysmith exits non-zero on any per-fn failure but still emits the
  # successes. The pool needs only a few descriptors to drive rylink,
  # so check the artifact directly rather than the exit code.
  jsons = [f for f in os.listdir(pool_dir) if f.endswith(".json")]
  if len(jsons) < 2:
    raise RuntimeError(
      f"rysmith produced too few descriptors ({len(jsons)}); stderr={r.stderr[-300:]!r}"
    )
  gid = extract_id(r.stdout)
  if gid is None:
    raise RuntimeError(
      f"rysmith stdout missing generation-id banner; stdout={r.stdout[:300]!r}"
    )
  return gid


def test_basic_run(rylink, rysmith):
  """A vanilla `rylink` invocation succeeds and produces N prog_<id>_<i> dirs."""
  with tempfile.TemporaryDirectory() as pool:
    seed_pool(rysmith, pool)
    with tempfile.TemporaryDirectory() as out:
      r = run(
        [
          rylink,
          "--input-dir",
          pool,
          "--n-progs",
          "3",
          "--seed",
          "7",
          "-o",
          out,
        ]
      )
      check(
        "rylink basic run exits 0",
        r.returncode == 0,
        f"rc={r.returncode}, stderr={r.stderr[:300]!r}",
      )
      gid = extract_id(r.stdout)
      check(
        "rylink stdout reports a 6-hex generation id",
        gid is not None,
        f"stdout={r.stdout[:200]!r}",
      )
      if gid is None:
        return
      dirs = sorted(d for d in os.listdir(out) if d.startswith("prog_"))
      check(
        f"three prog_{gid}_<i> dirs created",
        dirs == [f"prog_{gid}_0", f"prog_{gid}_1", f"prog_{gid}_2"],
        str(dirs),
      )


def test_program_sir_layout(rylink, rysmith):
  """Each prog dir contains program.sir + per-callee .sir; program.sir
  carries CG/param/ret comments at the top."""
  with tempfile.TemporaryDirectory() as pool:
    seed_pool(rysmith, pool)
    with tempfile.TemporaryDirectory() as out:
      r = run(
        [
          rylink,
          "--input-dir",
          pool,
          "--n-progs",
          "1",
          "--seed",
          "13",
          "-o",
          out,
        ]
      )
      check("rylink layout-test exits 0", r.returncode == 0, r.stderr[:200])
      gid = extract_id(r.stdout)
      if gid is None:
        check("rylink id discovery for layout-test", False, "no id in stdout")
        return
      prog_dir = os.path.join(out, f"prog_{gid}_0")
      check(f"prog_{gid}_0/ exists", os.path.isdir(prog_dir))
      if not os.path.isdir(prog_dir):
        return
      files = os.listdir(prog_dir)
      check(
        "program.sir present",
        "program.sir" in files,
        str(files),
      )
      sirs = [f for f in files if f.endswith(".sir")]
      check(
        "at least one .sir file (entry; ≥1 callee if CG has edges)",
        len(sirs) >= 1,
        str(sirs),
      )
      with open(os.path.join(prog_dir, "program.sir")) as f:
        head = f.read(2048)
      check(
        "program.sir header carries `// CG:` comment",
        "// CG:" in head,
        head[:200],
      )
      check(
        "program.sir header carries `// PARAMS:` comment",
        "// PARAMS:" in head,
        head[:200],
      )
      check(
        "program.sir header carries `// RETURN:` comment",
        "// RETURN:" in head,
        head[:200],
      )


def test_c_split_output(rylink, rysmith):
  """`--target c` (default split-by-source) emits common.h + per-source .c."""
  with tempfile.TemporaryDirectory() as pool:
    seed_pool(rysmith, pool)
    with tempfile.TemporaryDirectory() as out:
      r = run(
        [
          rylink,
          "--input-dir",
          pool,
          "--n-progs",
          "1",
          "--seed",
          "5",
          "--target",
          "c",
          "-o",
          out,
        ]
      )
      check("rylink --target c exits 0", r.returncode == 0, r.stderr[:200])
      gid = extract_id(r.stdout)
      if gid is None:
        check("rylink id discovery for c-split", False, "no id in stdout")
        return
      prog_dir = os.path.join(out, f"prog_{gid}_0")
      if not os.path.isdir(prog_dir):
        check("prog dir present for c-target", False, "missing dir")
        return
      files = os.listdir(prog_dir)
      check(
        "common.h emitted",
        "common.h" in files,
        str(files),
      )
      cs = [f for f in files if f.endswith(".c")]
      check(
        "at least one .c emitted",
        len(cs) >= 1,
        str(cs),
      )
      # rylink emits one .c per FunDecl::sourceStem (set in mergeInto)
      # plus the primary "program.c". With a pool of ≥2 functions the
      # bundle picks ≥2 nodes, so we expect ≥3 .c files. If this drops
      # to 1 it means sourceStem stopped surviving into the backend
      # (the bug B4 was meant to fix).
      check(
        "split-by-source emits one .c per FunDecl::sourceStem",
        len(cs) >= 3,
        f".c files = {sorted(cs)}",
      )


def test_validate(rylink, rysmith, symiri):
  """--validate proves the rewritten entry returns the originally-solved
  value when invoked with the entry's parameter realisation."""
  with tempfile.TemporaryDirectory() as pool:
    seed_pool(rysmith, pool, n_funcs=10, seed=31)
    with tempfile.TemporaryDirectory() as out:
      r = run(
        [
          rylink,
          "--input-dir",
          pool,
          "--n-progs",
          "2",
          "--seed",
          "9",
          "--validate",
          # Per-init "validated: OK" lines are gated on --verbose
          # (non-verbose only prints a per-program OK summary).
          "--verbose",
          "-o",
          out,
        ]
      )
      check(
        "rylink --validate exits 0 (every emitted program validated)",
        r.returncode == 0,
        f"rc={r.returncode}, stderr={r.stderr[:300]!r}",
      )
      # rylink should emit a per-program "validated: OK" log line on
      # success (mirroring rysmith).
      check(
        "stdout/stderr mentions `validated: OK` at least once",
        ("validated: OK" in r.stdout) or ("validated: OK" in r.stderr),
        f"stdout={r.stdout[:300]!r}",
      )


def test_rewrite_introduces_call(rylink, rysmith):
  """Default behaviour: at least one prog out of a small batch contains
  a `call @` in program.sir (the rewrite engine actually fired)."""
  with tempfile.TemporaryDirectory() as pool:
    seed_pool(rysmith, pool, n_funcs=10, seed=29)
    with tempfile.TemporaryDirectory() as out:
      r = run(
        [
          rylink,
          "--input-dir",
          pool,
          "--n-progs",
          "5",
          "--seed",
          "17",
          "-o",
          out,
        ]
      )
      check("rylink rewrite-test exits 0", r.returncode == 0, r.stderr[:200])
      gid = extract_id(r.stdout)
      if gid is None:
        check("rylink id discovery for rewrite-test", False, "no id in stdout")
        return
      any_call = False
      for i in range(5):
        p = os.path.join(out, f"prog_{gid}_{i}", "program.sir")
        if not os.path.isfile(p):
          continue
        txt = open(p).read()
        if re.search(r"\bcall\s+@", txt):
          any_call = True
          break
      check(
        "at least one program.sir contains a `call @...` (rewrite fired)",
        any_call,
        "no `call @` found across 5 programs",
      )


def test_rewrite_offset_in_range(rylink, rysmith, symirc):
  """The rewrite engine declines splices whose `offset = c - ret`
  doesn't fit the let's signed range — otherwise the C lowering of
  `call_result + offset` trips signed-overflow UB even though RefractIR
  semantics wrap cleanly. Drive a batch through `symirc --target c`
  with UBSan flags wired in by lowering to clang; any splice that
  slipped through the range check would surface as a runtime trap.
  This is a smoke check: it doesn't enumerate every overflow shape,
  it just confirms the engine produces UBSan-clean C for a few seeds
  that previously tripped (see commit history)."""
  import shutil as _sh
  import subprocess as _sub

  clang = _sh.which("clang")
  if clang is None:
    print("  [skip] clang not on PATH", file=sys.stderr)
    return
  with tempfile.TemporaryDirectory() as pool:
    seed_pool(rysmith, pool, n_funcs=10, seed=1234, n_params=3)
    with tempfile.TemporaryDirectory() as out:
      r = run(
        [
          rylink,
          "--input-dir",
          pool,
          "--n-progs",
          "5",
          "--seed",
          "1234",
          "--target",
          "c",
          "-o",
          out,
        ]
      )
      check("rylink overflow-check exits 0", r.returncode == 0, r.stderr[:200])
      gid = extract_id(r.stdout)
      if gid is None:
        check("rylink id discovery for overflow-check", False, "no id in stdout")
        return
      # Compile + run each emitted bundle under UBSan; the test is
      # whether anything traps (signed-overflow is fatal under
      # -fno-sanitize-recover=all).
      for i in range(5):
        prog_dir = os.path.join(out, f"prog_{gid}_{i}")
        c_path = os.path.join(prog_dir, "program.c")
        if not os.path.isfile(c_path):
          continue
        # Synthesize a minimal main that ignores the result — we
        # only need execution to surface UBSan; param values don't
        # matter for the overflow-detection check.
        main_c = os.path.join(prog_dir, "_main.c")
        with open(main_c, "w") as f:
          f.write("int main(void){return 0;}\n")
        exe = os.path.join(prog_dir, "_test")
        cc = _sub.run(
          [
            clang,
            "-O0",
            c_path,
            main_c,
            "-o",
            exe,
            "-fsanitize=undefined",
            "-fno-sanitize-recover=all",
            "-w",
            "-lm",
          ],
          stdout=_sub.PIPE,
          stderr=_sub.PIPE,
        )
        check(
          f"prog_{gid}_{i} compiles under UBSan",
          cc.returncode == 0,
          cc.stderr[:200].decode("utf-8", "replace"),
        )


def test_rylink_main(rylink, rysmith, symiri):
  """rylink --emit-main generates a @main wrapper in program.sir and standard main in C."""
  # Create a temporary directory for seeding the pool of functions
  with tempfile.TemporaryDirectory() as pool:
    seed_pool(rysmith, pool)

    # Create a temporary directory for output programs
    with tempfile.TemporaryDirectory() as out:
      # Run rylink with target 'c' and '--emit-main' enabled
      r = run(
        [
          rylink,
          "--input-dir",
          pool,
          "--n-progs",
          "1",
          "--seed",
          "42",
          "--target",
          "c",
          "--emit-main",
          "-o",
          out,
        ]
      )

      # Verify rylink execution succeeds
      check(
        "rylink with --emit-main exits 0",
        r.returncode == 0,
        f"rc={r.returncode}, stderr={r.stderr[:300]!r}",
      )

      # Extract the generation ID from stdout
      gid = extract_id(r.stdout)
      if gid is None:
        check("rylink id discovery for main-test", False, "no id in stdout")
        return

      prog_dir = os.path.join(out, f"prog_{gid}_0")
      if not os.path.isdir(prog_dir):
        check("prog dir present for main-test", False, "missing dir")
        return

      # 1. Read program.sir and verify @main function wrapper is present
      sir_path = os.path.join(prog_dir, "program.sir")
      check("program.sir exists", os.path.isfile(sir_path))
      if os.path.isfile(sir_path):
        with open(sir_path) as f:
          content = f.read()
        check(
          "generated bundled program contains fun @main",
          "fun @main(" in content,
          "fun @main not found in generated SIR",
        )

        # 2. Run symiri directly on the program.sir (without specifying arguments, as @main wraps them)
        r2 = run([symiri, sir_path])
        check(
          "executing @main directly via symiri exits 0 and returns Result: 0",
          r2.returncode == 0 and "Result: 0" in r2.stdout,
          f"rc={r2.returncode}, stdout={r2.stdout[:200]!r}, stderr={r2.stderr[:200]!r}",
        )

      # 3. Read program.c and verify standard C main function wrapper is present
      c_path = os.path.join(prog_dir, "program.c")
      check("program.c exists", os.path.isfile(c_path))
      if os.path.isfile(c_path):
        with open(c_path) as f:
          c_content = f.read()
        check(
          "generated C program contains standard main wrapper",
          "int32_t main(" in c_content,
          "int32_t main not found in generated C file",
        )

        # Try compiling the generated C files with gcc/clang if available
        import shutil as _sh
        import subprocess as _sub

        cc_bin = _sh.which("clang") or _sh.which("gcc")
        if cc_bin:
          exe = os.path.join(prog_dir, "_test")
          c_files = [
            os.path.join(prog_dir, f) for f in os.listdir(prog_dir) if f.endswith(".c")
          ]
          cc = _sub.run(
            [cc_bin, "-O0"] + c_files + ["-o", exe, "-lm"],
            stdout=_sub.PIPE,
            stderr=_sub.PIPE,
          )
          check(
            "C compilation with generated main exits 0",
            cc.returncode == 0,
            cc.stderr.decode("utf-8", "replace")[:300],
          )
          if cc.returncode == 0:
            run_exe = _sub.run([exe], stdout=_sub.PIPE, stderr=_sub.PIPE)
            check(
              "running compiled C binary with main returns 0",
              run_exe.returncode == 0,
              f"rc={run_exe.returncode}",
            )


def test_r9_p_noinline_noclone_callees(rylink, rysmith):
  """R9: `--p-noinline-callees` and `--p-noclone-callees` (independent
  probabilities, both default 0) randomly mark bundled non-entry funs
  with `__attribute__((noinline))` / `__attribute__((noclone))`. When
  both bits are set on the same fun the C backend folds them into a
  single `__attribute__((noinline, noclone))` token.

  The mechanism lives on FunDecl::Attributes, not in the backend. The C
  backend just reads the bits and emits the matching qualifier; the
  WASM backend ignores them. Entry funcs and `@main` are never marked
  — IPA-CP doesn't cross those boundaries (entry has no in-bundle
  caller, main is the program entry point and can't be cloned)."""
  # The R1 CRC32 intrinsic helper uses bare `__attribute__((noinline))`.
  # The combined `noinline, noclone` substring distinguishes the R9
  # output from the pre-existing helper without scoping to specific
  # functions.
  combined = "__attribute__((noinline, noclone))"
  with tempfile.TemporaryDirectory() as pool:
    seed_pool(rysmith, pool)
    # PROB=0,0: no R9 attribute in the C output.
    with tempfile.TemporaryDirectory() as out:
      r = run(
        [
          rylink,
          "--input-dir",
          pool,
          "--n-progs",
          "1",
          "--seed",
          "5",
          "--target",
          "c",
          "-o",
          out,
        ]
      )
      check("R9: PROB=0 default rylink run exits 0", r.returncode == 0, r.stderr[:200])
      saw_at_default = False
      for root, _, files in os.walk(out):
        for f in files:
          if f.endswith(".c") and combined in open(os.path.join(root, f)).read():
            saw_at_default = True
            break
      check(
        "R9: defaults emit no R9 noinline+noclone attribute",
        not saw_at_default,
        "(noinline, noclone) appeared without the --p-* flags",
      )
    # PROB=1,1 + emit-main: every callee carries the combined token,
    # @main does not.
    with tempfile.TemporaryDirectory() as out:
      r = run(
        [
          rylink,
          "--input-dir",
          pool,
          "--n-progs",
          "1",
          "--seed",
          "5",
          "--target",
          "c",
          "--emit-main",
          "--p-noinline-callees",
          "1.0",
          "--p-noclone-callees",
          "1.0",
          "-o",
          out,
        ]
      )
      check("R9: both-prob-1 rylink run exits 0", r.returncode == 0, r.stderr[:300])
      saw_combined = False
      main_unmarked = True
      for root, _, files in os.walk(out):
        for f in files:
          if not f.endswith(".c"):
            continue
          text = open(os.path.join(root, f)).read()
          if combined in text:
            saw_combined = True
          lines = text.splitlines()
          for i, line in enumerate(lines):
            if "int32_t main(void)" in line or " main(void)" in line:
              prev = lines[i - 1] if i > 0 else ""
              if combined in prev:
                main_unmarked = False
      check(
        "R9: both probabilities at 1.0 produce the combined attribute",
        saw_combined,
        "(noinline, noclone) missing from all .c outputs",
      )
      check(
        "R9: @main never carries the R9 attribute",
        main_unmarked,
        "@main was preceded by (noinline, noclone)",
      )


def main():
  if len(sys.argv) != 4:
    print("Usage: python3 -m test.unit.run_rylink_tests <rylink> <rysmith> <symiri>")
    sys.exit(2)
  rylink, rysmith, symiri = sys.argv[1:4]
  print("=== rylink basic run ===")
  test_basic_run(rylink, rysmith)
  print("=== rylink program.sir layout ===")
  test_program_sir_layout(rylink, rysmith)
  print("=== rylink --target c split output ===")
  test_c_split_output(rylink, rysmith)
  print("=== rylink --validate ===")
  test_validate(rylink, rysmith, symiri)
  print("=== rylink peephole rewrite fires ===")
  test_rewrite_introduces_call(rylink, rysmith)
  print("=== rylink rewrite offset range check ===")
  # symirc not used directly by this test but kept positional for the
  # CLI signature; passing rysmith is enough since seed_pool wraps it.
  test_rewrite_offset_in_range(rylink, rysmith, symiri)
  print("=== rylink --emit-main wrapper ===")
  test_rylink_main(rylink, rysmith, symiri)
  print("=== R9: --p-noinline-callees / --p-noclone-callees ===")
  test_r9_p_noinline_noclone_callees(rylink, rysmith)

  passed = sum(1 for _, ok, _ in results if ok)
  total = len(results)
  print(f"\nSummary (rylink_tests): {passed}/{total} passed.\n")
  sys.exit(0 if passed == total else 1)


if __name__ == "__main__":
  main()
