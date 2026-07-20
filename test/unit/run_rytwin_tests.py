#!/usr/bin/env python3
"""Unit tests for rytwin (equivalence-preserving RefractIR transformer).

Scaffold-level coverage: the CLI loads p1 + descriptor + state-profile
sidecar, runs the (currently empty) Pass pipeline, and emits an equivalent
p2. The headline invariant checked here is p1(i) == p2(i): symiri must
return the same value for the emitted program as for the input.

Usage: python3 -m test.unit.run_rytwin_tests <rytwin> <rysmith> <symiri>
"""

import os
import re
import subprocess
import sys
import tempfile

GREEN = "\033[92m"
RED = "\033[91m"
GRAY = "\033[90m"
NC = "\033[0m"

results = []


def run(cmd, **kw):
  print(f"  {GRAY}[RUN>]{NC} " + " ".join(str(c) for c in cmd))
  return subprocess.run(cmd, capture_output=True, text=True, timeout=120, **kw)


def check(name, ok, detail=""):
  results.append((name, ok, detail))
  color = GREEN if ok else RED
  tag = "PASS" if ok else "FAIL"
  print(f"  [{color}{tag}{NC}] {name}" + (f" — {detail}" if detail and not ok else ""))


def gen_p1(rysmith, d, seed="5", nparams="2"):
  """Generate one rysmith leaf with a state sidecar + descriptor. Returns
  (p1_path, desc_path, entry_func, [param_values]) or None on failure."""
  r = run(
    [
      rysmith,
      "--emit-state",
      "pbb",
      "--emit-desc",
      "--n-funcs",
      "1",
      "--seed",
      seed,
      "--n-params",
      nparams,
      "-o",
      d,
    ]
  )
  if r.returncode != 0:
    return None
  sirs = [f for f in os.listdir(d) if f.endswith(".sir") and "_sym" not in f]
  descs = [
    f for f in os.listdir(d) if f.endswith(".json") and not f.endswith(".state.json")
  ]
  if not sirs or not descs:
    return None
  p1 = os.path.join(d, sorted(sirs)[0])
  desc = os.path.join(d, sorted(descs)[0])
  src = open(p1).read()
  fm = re.search(r"fun\s+(@\w+)\s*\(([^)]*)\)", src)
  entry = fm.group(1)
  pnames = [p.split(":")[0].strip() for p in fm.group(2).split(",") if p.strip()]
  hdr = re.search(r"//\s*SOLVED:\s*(.*)", src)
  kv = {}
  if hdr:
    for part in hdr.group(1).split(","):
      if "=" in part:
        k, v = part.strip().split("=", 1)
        kv[k.strip()] = v.strip()
  args = [kv[p] for p in pnames if p in kv]
  return p1, desc, entry, args


def gen_pool(rysmith, d, n="12", seed="202", emit_state=False, extra=None):
  """Generate a pool of twin-friendly leaves (pointer-free until Stage 4).
  Returns the sorted list of concrete .sir paths, or None on failure."""
  cmd = [rysmith, "--emit-desc", "--n-funcs", n, "--seed", seed]
  cmd += ["--n-params", "2", "--n-stmts", "5", "--max-ptr-depth", "0"]
  if emit_state:
    cmd += ["--emit-state", "pbb"]
  cmd += (extra or []) + ["-o", d]
  r = run(cmd)
  if r.returncode != 0:
    return None
  sirs = [f for f in os.listdir(d) if f.endswith(".sir") and "_sym" not in f]
  return [os.path.join(d, s) for s in sorted(sirs)]


def first_twinned(rytwin, sirs, extra=None):
  """Run rytwin --p-twin 1.0 over the pool; return (p1, p2, stdout+stderr) of
  the first program that grafts, or None if none does."""
  for p1 in sirs:
    p2 = p1[:-4] + ".p2.sir"
    r = run([rytwin, p1, "--p-twin", "1.0", "--seed", "3"] + (extra or []) + ["-o", p2])
    if r.returncode == 0 and os.path.exists(p2):
      return p1, p2, r.stdout + r.stderr
  return None


def symiri_result(symiri, path, entry, args):
  r = run([symiri, "--main", entry, path, "--"] + args)
  out = r.stdout + r.stderr
  m = re.search(r"Result:\s*(\S+)", out)
  return (
    r.returncode,
    m.group(1) if m else None,
    out.strip().splitlines()[-1:] if out else [],
  )


def test_no_twins_errors(rytwin, rysmith):
  """When no twin is grafted (forced here via --p-twin 0), rytwin reports an
  error and writes no output rather than emitting an unchanged copy of p1."""
  with tempfile.TemporaryDirectory() as d:
    g = gen_p1(rysmith, d)
    if not g:
      check("no-twins setup (rysmith gen)", False, "generation failed")
      return
    p1, _, _, _ = g
    p2 = os.path.join(d, "p2.sir")
    r = run([rytwin, p1, "--p-twin", "0", "-o", p2])
    check("rytwin errors when no twin grafted", r.returncode != 0, f"rc={r.returncode}")
    check("no output written when no twin", not os.path.exists(p2), "p2 exists")
    check(
      "error explains the missing twin",
      "no twin" in r.stderr or "nothing written" in r.stderr,
      r.stderr[:160],
    )


def test_no_sidecar_needed(rytwin, rysmith):
  """[Stage 1] rytwin profiles p1 in-process: a p1 generated WITHOUT
  --emit-state (no .state.json on disk) still twins successfully."""
  with tempfile.TemporaryDirectory() as d:
    sirs = gen_pool(rysmith, d, emit_state=False)
    if not sirs:
      check("no-sidecar setup (rysmith gen)", False, "generation failed")
      return
    assert not any(f.endswith(".state.json") for f in os.listdir(d))
    got = first_twinned(rytwin, sirs)
    check("rytwin twins without a .state.json sidecar", got is not None, "")


def test_corrupt_sidecar_falls_back(rytwin, rysmith):
  """[Stage 1] A stale/corrupt .state.json next to p1 does not kill the run:
  rytwin warns and falls back to in-process profiling."""
  with tempfile.TemporaryDirectory() as d:
    sirs = gen_pool(rysmith, d, emit_state=True)
    if not sirs:
      check("corrupt-sidecar setup (rysmith gen)", False, "generation failed")
      return
    states = [f for f in os.listdir(d) if f.endswith(".state.json")]
    for f in states:
      open(os.path.join(d, f), "w").write("garbage{not json")
    got = first_twinned(rytwin, sirs)
    check("corrupt sidecar tolerated (twinning still works)", got is not None, "")


def strip_solved_header(path):
  src = open(path).read()
  open(path, "w").write(
    "\n".join(ln for ln in src.splitlines() if not ln.startswith("// SOLVED:")) + "\n"
  )


def test_sidecar_preferred(rytwin, rysmith):
  """[Stage 1] When a valid .state.json is present, rytwin loads it instead
  of interpreting. Discriminator: with the descriptor and SOLVED header
  removed, in-process profiling would run at all-zero args and trap on the
  interest requires — only the sidecar path can succeed."""
  with tempfile.TemporaryDirectory() as d:
    sirs = gen_pool(rysmith, d, emit_state=True)
    if not sirs:
      check("sidecar-preferred setup (rysmith gen)", False, "generation failed")
      return
    for f in os.listdir(d):
      if f.endswith(".json") and not f.endswith(".state.json"):
        os.remove(os.path.join(d, f))
    for p1 in sirs:
      strip_solved_header(p1)
    rescued = 0
    for p1 in sirs:
      state = p1[:-4] + ".state.json"
      if not os.path.exists(state):
        continue
      # Control: without the sidecar this program must fail (in-process
      # profiling at zero args traps); skip programs that pass at zeros.
      hidden = state + ".hidden"
      os.rename(state, hidden)
      r_ctl = run([rytwin, p1, "--p-twin", "1.0", "--seed", "3", "-o", p1 + ".ctl.sir"])
      os.rename(hidden, state)
      if r_ctl.returncode == 0:
        continue
      r = run([rytwin, p1, "--p-twin", "1.0", "--seed", "3", "-o", p1 + ".p2.sir"])
      if r.returncode == 0:
        rescued += 1
        break
    check("valid sidecar loaded in preference to interpreting", rescued > 0, "")


def test_solved_header_fallback(rytwin, rysmith, symiri):
  """[Stage 1] With no descriptor AND no sidecar, rytwin recovers the
  profiled input from p1's `// SOLVED:` header (a wrong input would trap on
  the interest requires during in-process profiling)."""
  with tempfile.TemporaryDirectory() as d:
    sirs = gen_pool(rysmith, d, emit_state=False)
    if not sirs:
      check("solved-header setup (rysmith gen)", False, "generation failed")
      return
    for f in os.listdir(d):
      if f.endswith(".json"):
        os.remove(os.path.join(d, f))
    got = first_twinned(rytwin, sirs, extra=["--validate"])
    check("rytwin twins from the SOLVED header alone", got is not None, "")
    if got:
      check("validated: OK without descriptor", "validated: OK" in got[2], got[2][:160])


def test_validate_without_sidecar(rytwin, rysmith):
  """[Stage 1] --validate works on a sidecar-free p1 (profiled input comes
  from the descriptor realization)."""
  with tempfile.TemporaryDirectory() as d:
    sirs = gen_pool(rysmith, d, emit_state=False)
    if not sirs:
      check("validate-no-sidecar setup (rysmith gen)", False, "generation failed")
      return
    got = first_twinned(rytwin, sirs, extra=["--validate"])
    check("--validate succeeds without a sidecar", got is not None, "")
    if got:
      check("validated: OK without sidecar", "validated: OK" in got[2], got[2][:160])


def test_bad_guard_rejected(rytwin, rysmith):
  """--guard must be sum|crc32."""
  with tempfile.TemporaryDirectory() as d:
    g = gen_p1(rysmith, d)
    if not g:
      check("rytwin bad-guard setup", False, "generation failed")
      return
    p1, desc, _, _ = g
    p2 = os.path.join(d, "p2.sir")
    r = run([rytwin, p1, "--guard", "bogus", "-o", p2])
    check("invalid --guard rejected (rc != 0)", r.returncode != 0, f"rc={r.returncode}")


def test_missing_args_usage(rytwin):
  """No input / no output prints usage with a non-zero exit."""
  r = run([rytwin])
  check("missing input/output → non-zero exit", r.returncode != 0, f"rc={r.returncode}")


def parse_entry(src):
  fm = re.search(r"fun\s+(@\w+)\s*\(([^)]*)\)", src)
  params = [p.split(":") for p in fm.group(2).split(",") if p.strip()]
  pnames = [p[0].strip() for p in params]
  ptypes = [p[1].strip() for p in params]
  hdr = re.search(r"//\s*SOLVED:\s*(.*)", src)
  kv = {}
  if hdr:
    for part in hdr.group(1).split(","):
      if "=" in part:
        k, v = part.strip().split("=", 1)
        kv[k.strip()] = v.strip()
  return fm.group(1), pnames, ptypes, [kv.get(p, "0") for p in pnames]


def equivalence_over_pool(rytwin, symiri, d, sirs, tag):
  """Twin every program in the pool with --p-twin 1.0 and assert p1 === p2 on
  the profiled input AND on other inputs (where the guard does not fire)."""
  import random

  rng = random.Random(1)
  twinned = prof_bad = other_bad = other_checks = 0
  for p1 in sirs:
    stem = os.path.basename(p1)[:-4]
    desc = os.path.join(d, re.sub(r"[a-z]$", "", stem) + ".json")
    if not os.path.exists(desc):
      continue
    fn, pnames, ptypes, iargs = parse_entry(open(p1).read())
    p2 = os.path.join(d, stem + ".p2.sir")
    rr = run([rytwin, p1, "--p-twin", "1.0", "--seed", "3", "-o", p2])
    if rr.returncode != 0:
      # A block-free-of-eligible-scalars program now exits non-zero with a
      # "no twin" message — expected, skip it. Anything else is a failure.
      if "no twin" in rr.stderr or "nothing written" in rr.stderr:
        continue
      check(f"rytwin ran on {stem} [{tag}]", False, rr.stderr[:160])
      return
    twinned += 1
    if (
      symiri_result(symiri, p1, fn, iargs)[1:]
      != symiri_result(symiri, p2, fn, iargs)[1:]
    ):
      prof_bad += 1
    # Other inputs (only when all params are integer) — the exact guard
    # must keep the equivalence even where the guard does not fire.
    if all("f" not in t for t in ptypes):
      for _ in range(4):
        a = [str(rng.randint(-1_000_000, 1_000_000)) for _ in pnames]
        other_checks += 1
        if symiri_result(symiri, p1, fn, a)[1:] != symiri_result(symiri, p2, fn, a)[1:]:
          other_bad += 1
  check(
    f"TwinPass grafted at least one twin [{tag}]", twinned > 0, f"twinned={twinned}"
  )
  check(
    f"every twin preserves the profiled result [{tag}]",
    prof_bad == 0,
    f"{prof_bad} mismatch(es)",
  )
  check(
    f"every twin preserves results on other inputs [{tag}]",
    other_bad == 0,
    f"{other_bad}/{other_checks} mismatch(es)",
  )


def test_twinpass_grafts_and_preserves_equivalence(rytwin, rysmith, symiri):
  """On pointer-free programs (scalars, structs, arrays, vectors and pure
  intrinsic calls — everything eligibility now covers), TwinPass grafts twins
  whose exact guard keeps p1 === p2: identical results on the profiled input
  AND on other inputs."""
  with tempfile.TemporaryDirectory() as d:
    # Pointers/memory aren't twin candidates yet, so disable them; keep
    # aggregates, vectors and intrinsic calls to exercise the lifted
    # eligibility.
    sirs = gen_pool(rysmith, d, emit_state=True)
    if sirs is None:
      check("twinpass setup (scalar-only gen)", False, "generation failed")
      return
    equivalence_over_pool(rytwin, symiri, d, sirs, "sidecar")


def test_equivalence_without_sidecar(rytwin, rysmith, symiri):
  """[Stage 1] The full equivalence suite holds when p1 is generated without
  --emit-state — the profile rytwin keys its guards on is computed
  in-process."""
  with tempfile.TemporaryDirectory() as d:
    sirs = gen_pool(rysmith, d, emit_state=False)
    if sirs is None:
      check("no-sidecar equivalence setup", False, "generation failed")
      return
    equivalence_over_pool(rytwin, symiri, d, sirs, "no-sidecar")


def test_validate_and_target(rytwin, rysmith, symiri):
  """--validate asserts p1 === p2 in-process, and --target c compiles p2."""
  with tempfile.TemporaryDirectory() as d:
    r = run(
      [
        rysmith,
        "--emit-state",
        "pbb",
        "--emit-desc",
        "--emit-main",
        "--n-funcs",
        "12",
        "--seed",
        "100",
        "--n-params",
        "2",
        "--n-stmts",
        "4",
        "--max-ptr-depth",
        "0",
        "--no-vec",
        "--no-agg-ptr",
        "--no-intrinsics",
        "-o",
        d,
      ]
    )
    if r.returncode != 0:
      check("validate/target setup", False, r.stderr[:200])
      return
    # Find a program that actually grafts a twin.
    picked = None
    for s in sorted(f for f in os.listdir(d) if f.endswith(".sir") and "_sym" not in f):
      p1 = os.path.join(d, s)
      p2 = os.path.join(d, s[:-4] + ".p2.sir")
      rr = run([rytwin, p1, "--p-twin", "1.0", "--seed", "3", "-o", p2])
      m = re.search(r"\((\d+) twin", rr.stdout)
      if rr.returncode == 0 and m and int(m.group(1)) > 0:
        picked = p1
        break
    check("found a twinnable program", picked is not None, "")
    if not picked:
      return
    p2 = os.path.join(d, "pv.sir")
    r = run(
      [
        rytwin,
        picked,
        "--p-twin",
        "1.0",
        "--seed",
        "3",
        "--validate",
        "--target",
        "c",
        "--emit-main",
        "-o",
        p2,
      ]
    )
    check("rytwin --validate --target c exits 0", r.returncode == 0, r.stderr[:200])
    check("rytwin reports validated: OK", "validated: OK" in r.stdout, r.stdout[:200])
    check("rytwin emitted p2.c", os.path.exists(os.path.join(d, "pv.c")), "")


def main():
  if len(sys.argv) not in (4, 5):
    print(
      "Usage: python3 -m test.unit.run_rytwin_tests <rytwin> <rysmith> <symiri>"
      " [test-name-substring]"
    )
    sys.exit(2)
  rytwin, rysmith, symiri = sys.argv[1:4]
  only = sys.argv[4] if len(sys.argv) == 5 else ""

  tests = [
    (
      "rytwin: no twin grafted -> error, no output",
      lambda: test_no_twins_errors(rytwin, rysmith),
    ),
    (
      "rytwin: no sidecar needed (in-process profiling)",
      lambda: test_no_sidecar_needed(rytwin, rysmith),
    ),
    (
      "rytwin: corrupt sidecar falls back to in-process profiling",
      lambda: test_corrupt_sidecar_falls_back(rytwin, rysmith),
    ),
    (
      "rytwin: valid sidecar preferred over interpreting",
      lambda: test_sidecar_preferred(rytwin, rysmith),
    ),
    (
      "rytwin: SOLVED-header fallback",
      lambda: test_solved_header_fallback(rytwin, rysmith, symiri),
    ),
    (
      "rytwin: --validate without sidecar",
      lambda: test_validate_without_sidecar(rytwin, rysmith),
    ),
    (
      "rytwin: invalid --guard rejected",
      lambda: test_bad_guard_rejected(rytwin, rysmith),
    ),
    ("rytwin: missing args usage", lambda: test_missing_args_usage(rytwin)),
    (
      "TwinPass: grafts twins, preserves equivalence (profiled + other inputs)",
      lambda: test_twinpass_grafts_and_preserves_equivalence(rytwin, rysmith, symiri),
    ),
    (
      "TwinPass: equivalence without sidecar",
      lambda: test_equivalence_without_sidecar(rytwin, rysmith, symiri),
    ),
    (
      "rytwin: --validate and --target c",
      lambda: test_validate_and_target(rytwin, rysmith, symiri),
    ),
  ]
  for title, fn in tests:
    if only and only not in title and only not in fn.__code__.co_names[0]:
      continue
    print(f"=== {title} ===")
    fn()

  passed = sum(1 for _, ok, _ in results if ok)
  total = len(results)
  print(f"\nSummary (rytwin_tests): {passed}/{total} passed.\n")
  sys.exit(0 if passed == total else 1)


if __name__ == "__main__":
  main()
