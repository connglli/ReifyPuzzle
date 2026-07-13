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


def symiri_result(symiri, path, entry, args):
  r = run([symiri, "--main", entry, path, "--"] + args)
  out = r.stdout + r.stderr
  m = re.search(r"Result:\s*(\S+)", out)
  return (
    r.returncode,
    m.group(1) if m else None,
    out.strip().splitlines()[-1:] if out else [],
  )


def test_scaffold_roundtrip(rytwin, rysmith, symiri):
  """rytwin emits an equivalent p2: symiri(p2, i) == symiri(p1, i)."""
  with tempfile.TemporaryDirectory() as d:
    g = gen_p1(rysmith, d)
    if not g:
      check("rytwin roundtrip setup (rysmith gen)", False, "generation failed")
      return
    p1, desc, entry, args = g
    p2 = os.path.join(d, "p2.sir")
    r = run([rytwin, p1, "--desc", desc, "-o", p2])
    check(
      "rytwin exits 0", r.returncode == 0, f"rc={r.returncode}, err={r.stderr[:200]!r}"
    )
    check("rytwin reports it wrote p2", "wrote" in r.stdout, r.stdout[:160])
    check(
      "rytwin emitted no warnings", "warning" not in r.stderr.lower(), r.stderr[:200]
    )
    check("p2.sir exists", os.path.exists(p2), "")
    if not os.path.exists(p2):
      return
    rc1, v1, t1 = symiri_result(symiri, p1, entry, args)
    rc2, v2, t2 = symiri_result(symiri, p2, entry, args)
    check("p1 runs cleanly", rc1 == 0 and v1 is not None, f"rc={rc1} {t1}")
    check(
      f"p1(i)==p2(i): {v1} == {v2}",
      rc1 == rc2 and v1 == v2 and v1 is not None,
      f"p1=({rc1},{v1}) p2=({rc2},{v2})",
    )


def test_state_profile_inferred_and_parsed(rytwin, rysmith):
  """With --state omitted, rytwin infers <stem>.state.json and parses it —
  no 'could not parse' / 'not found' warning."""
  with tempfile.TemporaryDirectory() as d:
    g = gen_p1(rysmith, d)
    if not g:
      check("rytwin state-profile setup", False, "generation failed")
      return
    p1, desc, _, _ = g
    p2 = os.path.join(d, "p2.sir")
    r = run([rytwin, p1, "--desc", desc, "-o", p2])
    check(
      "inferred state profile parsed without warning",
      "could not parse state profile" not in r.stderr and "not found" not in r.stderr,
      r.stderr[:200],
    )


def test_bad_guard_rejected(rytwin, rysmith):
  """--guard must be sum|crc32."""
  with tempfile.TemporaryDirectory() as d:
    g = gen_p1(rysmith, d)
    if not g:
      check("rytwin bad-guard setup", False, "generation failed")
      return
    p1, desc, _, _ = g
    p2 = os.path.join(d, "p2.sir")
    r = run([rytwin, p1, "--desc", desc, "--guard", "bogus", "-o", p2])
    check("invalid --guard rejected (rc != 0)", r.returncode != 0, f"rc={r.returncode}")


def test_missing_args_usage(rytwin):
  """No input / no output prints usage with a non-zero exit."""
  r = run([rytwin])
  check("missing input/output → non-zero exit", r.returncode != 0, f"rc={r.returncode}")


def main():
  if len(sys.argv) != 4:
    print("Usage: python3 -m test.unit.run_rytwin_tests <rytwin> <rysmith> <symiri>")
    sys.exit(2)
  rytwin, rysmith, symiri = sys.argv[1:4]
  print("=== rytwin scaffold: p1 -> p2 round-trip equivalence ===")
  test_scaffold_roundtrip(rytwin, rysmith, symiri)
  print("=== rytwin: state profile inferred + parsed ===")
  test_state_profile_inferred_and_parsed(rytwin, rysmith)
  print("=== rytwin: invalid --guard rejected ===")
  test_bad_guard_rejected(rytwin, rysmith)
  print("=== rytwin: missing args usage ===")
  test_missing_args_usage(rytwin)

  passed = sum(1 for _, ok, _ in results if ok)
  total = len(results)
  print(f"\nSummary (rytwin_tests): {passed}/{total} passed.\n")
  sys.exit(0 if passed == total else 1)


if __name__ == "__main__":
  main()
