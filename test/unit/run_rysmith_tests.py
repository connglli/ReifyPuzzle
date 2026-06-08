"""End-to-end tests for rysmith's v0.2.2 generation surface:

  (1) Every run prints a 6-hex-char generation ID derived from --seed
      and prefixes every generated function and struct with it.
  (2) `--n-params N` adds N scalar parameters to each generated
      function. Parameters appear in the function signature and
      are usable as RValues throughout the body.
  (3) Every successful generation writes a `func_<id>_<i>.json`
      descriptor with the canonical schema.
  (4) The concrete .sir output carries a `// SOLVED:` header naming
      the solved parameter values, which `symiri ... -- <vals>`
      replays bit-exact.
  (5) R3 assignment-shape invariants: the generator never emits
      `%x = %x;` (self-assigns), plain `%x = %y;` copies for
      scalar/vec LHS, or any pattern where the LHS token reappears
      on the RHS. These shapes fold flat under SCCP / IPA-CP and
      were the bulk of pre-R3's optimization surface.

Run as:

  python3 -m test.lib.run_rysmith_tests <rysmith> <symiri>

Each test prints PASS or FAIL; exit code reflects the worst result.
"""

import json
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

# rysmith emits a `generation id = <hex>` banner on stdout — every test
# below relies on this line because --id was removed. Same regex is
# duplicated in run_rylink_tests.py to keep each module standalone.
_ID_RE = re.compile(r"generation id\s*=\s*([0-9a-f]{6})")


def run(cmd, **kw):
  print(f"  {GRAY}[RUN>]{NC} " + " ".join(cmd))
  return subprocess.run(cmd, capture_output=True, text=True, timeout=60, **kw)


def check(name, ok, detail=""):
  results.append((name, ok, detail))
  color = GREEN if ok else RED
  tag = "PASS" if ok else "FAIL"
  print(f"  [{color}{tag}{NC}] {name}" + (f" — {detail}" if detail and not ok else ""))


def extract_id(stdout):
  m = _ID_RE.search(stdout or "")
  return m.group(1) if m else None


def test_id_banner(rysmith):
  """rysmith prints a 6-hex-char generation ID and uses it for every
  generated function and struct."""
  with tempfile.TemporaryDirectory() as d:
    r = run(
      [
        rysmith,
        "--n-funcs",
        "2",
        "--seed",
        "42",
        "--emit-desc",
        "-o",
        d,
      ]
    )
    check(
      "rysmith default-id run exits 0",
      r.returncode == 0,
      f"rc={r.returncode}, stderr={r.stderr[:300]!r}",
    )
    gid = extract_id(r.stdout)
    check(
      "stdout reports a 6-hex generation id",
      gid is not None,
      f"stdout={r.stdout[:200]!r}",
    )
    if gid is None:
      return
    files = sorted(os.listdir(d))
    expected_sirs = {f"func_{gid}_0", f"func_{gid}_1"}
    sir_stems = {f.rsplit(".", 1)[0] for f in files if f.endswith(".sir")}
    # Single-init produces `func_<id>_<i>.sir`; multi-init produces
    # `func_<id>_<i><a..z>.sir`. Trim a trailing init-letter to
    # recover the base name in both cases.
    base_stems = set()
    for s in sir_stems:
      m = re.match(rf"(func_{gid}_\d+)[a-z]?$", s)
      if m:
        base_stems.add(m.group(1))
    check(
      f"both func_{gid}_<i> base names present",
      expected_sirs.issubset(base_stems),
      f"base={base_stems}",
    )
    json_names = sorted(f for f in files if f.endswith(".json"))
    check(
      f"descriptors func_{gid}_0.json and func_{gid}_1.json emitted",
      json_names == [f"func_{gid}_0.json", f"func_{gid}_1.json"],
      str(json_names),
    )


def test_seed_id_determinism(rysmith):
  """Two runs with the same --seed produce the same generation ID."""
  with tempfile.TemporaryDirectory() as d1, tempfile.TemporaryDirectory() as d2:
    r1 = run([rysmith, "--n-funcs", "1", "--seed", "777", "-o", d1])
    r2 = run([rysmith, "--n-funcs", "1", "--seed", "777", "-o", d2])
    g1 = extract_id(r1.stdout)
    g2 = extract_id(r2.stdout)
    check(
      "same --seed yields the same generation id across runs",
      g1 is not None and g1 == g2,
      f"g1={g1!r} g2={g2!r}",
    )


def test_n_params(rysmith):
  """--n-params 3 puts three scalar parameters into every generated function."""
  with tempfile.TemporaryDirectory() as d:
    r = run(
      [
        rysmith,
        "--n-funcs",
        "1",
        "--emit-desc",
        "--seed",
        "7",
        "--n-params",
        "3",
        "-o",
        d,
      ]
    )
    check(
      "rysmith --n-params=3 exits 0", r.returncode == 0, f"stderr={r.stderr[:200]!r}"
    )
    gid = extract_id(r.stdout)
    if gid is None:
      check("rysmith id discovery for --n-params", False, "no id in stdout")
      return
    desc_path = os.path.join(d, f"func_{gid}_0.json")
    check("descriptor exists", os.path.isfile(desc_path))
    if not os.path.isfile(desc_path):
      return
    desc = json.load(open(desc_path))
    check(
      "descriptor lists 3 params",
      len(desc.get("params", [])) == 3,
      f"params={desc.get('params')}",
    )
    names = [p["name"] for p in desc.get("params", [])]
    check("param names are %pa0..%pa2", names == ["%pa0", "%pa1", "%pa2"], str(names))
    types = [p["type"] for p in desc.get("params", [])]
    scalar_pat = re.compile(r"^(i\d+|f32|f64)$")
    check(
      "all params are scalar types", all(scalar_pat.match(t) for t in types), str(types)
    )
    # Body actually uses each param somewhere (or at least references them
    # — rysmith may not always pick them when many other vars are around)
    sir_path = next(
      (
        os.path.join(d, f)
        for f in os.listdir(d)
        if f.startswith(f"func_{gid}_0") and f.endswith(".sir")
      ),
      None,
    )
    check("at least one concrete .sir emitted", sir_path is not None)
    if sir_path:
      body = open(sir_path).read()
      check(
        "signature uses %pa0..%pa2",
        all(p in body for p in ["%pa0", "%pa1", "%pa2"]),
        body[:200],
      )


def test_descriptor_schema(rysmith):
  """Descriptor JSON parses and carries every documented field."""
  with tempfile.TemporaryDirectory() as d:
    r = run(
      [
        rysmith,
        "--n-funcs",
        "1",
        "--emit-desc",
        "--seed",
        "11",
        "--n-params",
        "2",
        "-o",
        d,
      ]
    )
    if r.returncode != 0:
      check("rysmith descriptor-schema setup", False, r.stderr[:200])
      return
    gid = extract_id(r.stdout)
    if gid is None:
      check("rysmith id discovery for descriptor-schema", False, "no id in stdout")
      return
    desc = json.load(open(os.path.join(d, f"func_{gid}_0.json")))
    for k in (
      "id",
      "name",
      "ret_type",
      "params",
      "path",
      "structs",
      "realizations",
    ):
      check(f"descriptor field `{k}` present", k in desc, str(list(desc.keys())))
    # Top-level `syms` was dropped (each realization has its own
    # potentially-different sym set; the old top-level field reflected
    # only the last init and was misleading).
    check("descriptor.syms removed", "syms" not in desc, str(list(desc.keys())))
    check("descriptor.id matches stdout id", desc.get("id") == gid, desc.get("id"))
    check(
      f"name is @func_{gid}_0",
      desc.get("name") == f"@func_{gid}_0",
      desc.get("name"),
    )
    rzs = desc.get("realizations")
    check(
      "at least one realization listed",
      isinstance(rzs, list) and rzs,
      str(rzs),
    )
    if rzs:
      rz = rzs[0]
      check("realization has file", "file" in rz, str(rz))
      check("realization has params dict", isinstance(rz.get("params"), dict), str(rz))
      check("realization has syms dict", isinstance(rz.get("syms"), dict), str(rz))
      check("realization has ret", "ret" in rz, str(rz))
      # All declared params have a value
      param_names = [p["name"] for p in desc.get("params", [])]
      check(
        "every declared param has a solved value",
        all(n in rz.get("params", {}) for n in param_names),
        f"params={rz.get('params')}",
      )


def test_solved_replay(rysmith, symiri):
  """SOLVED header round-trips through symiri positional args."""
  with tempfile.TemporaryDirectory() as d:
    r = run(
      [
        rysmith,
        "--n-funcs",
        "1",
        "--emit-desc",
        "--seed",
        "13",
        "--n-params",
        "2",
        "-o",
        d,
      ]
    )
    if r.returncode != 0:
      check("rysmith solved-replay setup", False, r.stderr[:200])
      return
    gid = extract_id(r.stdout)
    if gid is None:
      check("rysmith id discovery for solved-replay", False, "no id in stdout")
      return
    sirs = sorted(
      f for f in os.listdir(d) if f.startswith(f"func_{gid}_0") and f.endswith(".sir")
    )
    if not sirs:
      check("at least one concrete .sir for replay", False, "no sirs")
      return
    sir_path = os.path.join(d, sirs[0])
    header = open(sir_path).readline().strip()
    check("first line is // SOLVED:", header.startswith("// SOLVED:"), header)
    if not header.startswith("// SOLVED:"):
      return
    kv = dict(re.findall(r"(%\w+|ret)=(-?\d+(?:\.\d+(?:[eE][-+]?\d+)?)?)", header))
    pa0 = kv.get("%pa0")
    pa1 = kv.get("%pa1")
    ret = kv.get("ret")
    check(
      "SOLVED has %pa0, %pa1, ret",
      pa0 is not None and pa1 is not None and ret is not None,
      str(kv),
    )
    if pa0 is None or pa1 is None:
      return
    # Replay through symiri — must produce the same return value.
    r2 = run([symiri, "--main", f"@func_{gid}_0", sir_path, "--", pa0, pa1])
    expected = f"Result: {ret.rstrip('.0')}" if ret.endswith(".0") else f"Result: {ret}"
    # symiri prints int returns as bare decimal; tolerate equality on the int.
    ok = r2.returncode == 0 and (f"Result: {ret}" in r2.stdout or expected in r2.stdout)
    check(
      f"symiri replay @func_{gid}_0({pa0},{pa1}) -> {ret}",
      ok,
      f"rc={r2.returncode}, stdout={r2.stdout[:120]!r}",
    )


def test_rysmith_main(rysmith, symiri):
  """rysmith --emit-main generates a @main wrapper that we can execute directly via symiri."""
  with tempfile.TemporaryDirectory() as d:
    r = run(
      [
        rysmith,
        "--n-funcs",
        "1",
        "--seed",
        "42",
        "--n-params",
        "2",
        "--emit-main",
        "-o",
        d,
      ]
    )
    check(
      "rysmith --emit-main run exits 0",
      r.returncode == 0,
      f"rc={r.returncode}, stderr={r.stderr[:300]!r}",
    )
    gid = extract_id(r.stdout)
    if gid is None:
      return
    sirs = [f for f in os.listdir(d) if f.endswith(".sir")]
    if not sirs:
      check("at least one concrete .sir for main test", False, "no sirs")
      return
    sir_path = os.path.join(d, sirs[0])

    # Read file and verify @main is present
    with open(sir_path) as f:
      content = f.read()
    check(
      "generated program contains fun @main",
      "fun @main(" in content,
      "fun @main not found in generated SIR",
    )

    # Execute symiri on it directly without entry or parameters!
    r2 = run([symiri, sir_path])
    check(
      "executing @main directly via symiri exits 0 and returns Result: 0",
      r2.returncode == 0 and "Result: 0" in r2.stdout,
      f"rc={r2.returncode}, stdout={r2.stdout[:200]!r}, stderr={r2.stderr[:200]!r}",
    )


# ---------------------------------------------------------------------------
# R3 assignment-shape invariants
# ---------------------------------------------------------------------------
#
# rysmith's RHS generator (src/reify/expr_gen.cpp) filters the LHS local
# name out of every variable-pool pick so reductive patterns can't appear
# in the solved output. The plan + rationale lives in tmp/reify_effectiveness_plan.md.
# These tests sample a band of seeds, walk every emitted body assignment,
# and assert the four invariants below.

# Seeds chosen to exercise a variety of programs without overlapping the
# seeds used by the v0.2.2-surface tests above.
_R3_SEEDS = [1001, 1002, 1003, 1004, 1005, 1006]

# `%v0`, `%pa1`, `%pp2`, `%t0`, `%a0`, etc. — any local sigil.
_R3_LOCAL = r"%[A-Za-z_][A-Za-z0-9_]*"
# A simple LValue at the start of a line: `LHS = RHS;` with no field/index
# accesses on LHS. Aggregate LHS (`%a[i] = …`) is excluded because the
# root may legitimately appear in RHS at a different access path.
_R3_SIMPLE_LHS = re.compile(rf"^\s*({_R3_LOCAL})\s*=\s*(.+);\s*$")
_R3_BARE_LOCAL_RHS = re.compile(rf"^({_R3_LOCAL})$")
# rysmith naming convention (see src/reify/var_catalogue.cpp): %v scalar,
# %vec vector, %a array, %t struct, %p ptr, %pp ptr-to-ptr, %ap aggregate
# ptr, %pa param scalar. The plain-copy test exempts pointer-typed LHS:
# `%p0 = %p1;` is a meaningful aliasing operation and the alternative
# (e.g. `%p0 = %p1 + 1;`) would push %p0 past the single-element object
# it points to and cause UB on any subsequent `load %p0`.
_R3_PTR_LHS = re.compile(r"^%(p|pp|ap)\d")


def _r3_collect_sirs(rysmith, seeds, n_funcs=4):
  """Generate programs with the given seeds and return a list of .sir paths."""
  sirs = []
  for s in seeds:
    d = tempfile.mkdtemp(prefix="r3_")
    r = run([rysmith, "--n-funcs", str(n_funcs), "--seed", str(s), "-o", d])
    if r.returncode != 0:
      print(f"  {RED}rysmith failed for seed {s}: {r.stderr[:200]!r}{NC}")
      continue
    for f in os.listdir(d):
      if f.endswith(".sir"):
        sirs.append(os.path.join(d, f))
  return sirs


def _r3_body_assign_lines(sir_path):
  """Yield body assignment lines from a .sir file, skipping the synthesized
  CRC32 exit-block chain and the @main wrapper (both produced by reify
  rather than the user-code generator)."""
  with open(sir_path) as f:
    text = f.read()
  in_main = False
  for line in text.splitlines():
    stripped = line.strip()
    if not stripped or stripped.startswith("//"):
      continue
    if stripped.startswith("fun @main"):
      in_main = True
      continue
    if stripped.startswith("fun @") and not stripped.startswith("fun @main"):
      in_main = False
      continue
    if in_main:
      continue
    # Skip the CRC32 exit-block chain (all lines touch %_chk only).
    if "%_chk" in stripped:
      continue
    if "=" not in stripped or not stripped.endswith(";"):
      continue
    if stripped.startswith("let "):
      continue
    yield stripped


def _r3_split_lhs_rhs(line):
  m = _R3_SIMPLE_LHS.match(line)
  if not m:
    return None, None
  return m.group(1), m.group(2).strip()


def test_r3_no_self_assign(rysmith):
  """R3: `%x = %x;` should never appear."""
  sirs = _r3_collect_sirs(rysmith, _R3_SEEDS)
  bad = []
  for sir in sirs:
    for line in _r3_body_assign_lines(sir):
      lhs, rhs = _r3_split_lhs_rhs(line)
      if lhs is None:
        continue
      m = _R3_BARE_LOCAL_RHS.match(rhs)
      if m and m.group(1) == lhs:
        bad.append((sir, line))
  check(
    f"R3: 0 scalar/ptr/vec self-assigns across {len(sirs)} .sir files",
    not bad,
    f"first violation: {bad[0]}" if bad else "",
  )


def test_r3_no_plain_copy(rysmith):
  """R3: `%x = %y;` (plain copy, both bare locals, x != y) should never
  appear for scalar/vec LHS. Pointer LHS is exempted — `%p0 = %p1;` is a
  meaningful aliasing op and the obvious "fix" (`%p1 + 1`) would step a
  pointer past its single-element object and trip load UB."""
  sirs = _r3_collect_sirs(rysmith, _R3_SEEDS)
  bad = []
  for sir in sirs:
    for line in _r3_body_assign_lines(sir):
      lhs, rhs = _r3_split_lhs_rhs(line)
      if lhs is None or _R3_PTR_LHS.match(lhs):
        continue
      m = _R3_BARE_LOCAL_RHS.match(rhs)
      if m and m.group(1) != lhs:
        bad.append((sir, line))
  check(
    f"R3: 0 plain bare-RValueAtom copies (scalar/vec LHS) across {len(sirs)} .sir files",
    not bad,
    f"first violation: {bad[0]}" if bad else "",
  )


def test_r3_no_lhs_in_rhs(rysmith):
  """R3: the LHS variable token must not appear anywhere in the RHS of a
  simple-LHS assignment. Catches `%v3 = %v3 - 4 * %v3 + 4 * %v3;` and
  similar reductive patterns. `addr`/`load` forms are exempted because
  they index different pools that R3 intentionally leaves untouched."""
  sirs = _r3_collect_sirs(rysmith, _R3_SEEDS)
  bad = []
  for sir in sirs:
    for line in _r3_body_assign_lines(sir):
      lhs, rhs = _r3_split_lhs_rhs(line)
      if lhs is None:
        continue
      if rhs.startswith("addr ") or rhs.startswith("load "):
        continue
      pattern = re.compile(rf"(?<![A-Za-z0-9_]){re.escape(lhs)}(?![A-Za-z0-9_])")
      if pattern.search(rhs):
        bad.append((sir, line))
  check(
    f"R3: 0 LHS-appears-in-RHS violations across {len(sirs)} .sir files",
    not bad,
    f"first violation: {bad[0]}" if bad else "",
  )


def test_r3_baseline_assignment_volume(rysmith):
  """R3 sanity guard: the harness actually saw a meaningful number of
  body assignments. Without this, the "0 violations" tests pass
  trivially if rysmith stops emitting code."""
  sirs = _r3_collect_sirs(rysmith, _R3_SEEDS)
  n = 0
  for sir in sirs:
    for _ in _r3_body_assign_lines(sir):
      n += 1
  check(
    f"R3: >= 200 body assignments observed across {len(sirs)} .sir files",
    n >= 200,
    f"only {n} body assignments",
  )


def test_r3_pa_param_uses_present(rysmith):
  """R3 spirit-check: plain-copy elimination must not starve out param
  usage. After R3 we still expect non-trivial expressions involving
  `%paN` (e.g. `%v0 = %pa1 - 4;` or `%v0 = sym * %pa1`) — otherwise
  the generator may have degenerated into literal-only RHSs."""
  sirs = _r3_collect_sirs(rysmith, _R3_SEEDS)
  param_uses = 0
  for sir in sirs:
    for line in _r3_body_assign_lines(sir):
      lhs, rhs = _r3_split_lhs_rhs(line)
      if lhs is None:
        continue
      if re.search(r"%pa\d", rhs) and re.search(r"[+\-*/%&|^<>]", rhs):
        param_uses += 1
  check(
    f"R3: >= 1 non-trivial RHS uses of a %paN param across {len(sirs)} files",
    param_uses >= 1,
    f"only {param_uses} non-trivial param uses observed",
  )


def main():
  if len(sys.argv) != 3:
    print("Usage: python3 -m test.lib.run_rysmith_tests <rysmith> <symiri>")
    sys.exit(2)
  rysmith, symiri = sys.argv[1:3]
  print("=== rysmith generation-id banner ===")
  test_id_banner(rysmith)
  print("=== rysmith --seed determinism ===")
  test_seed_id_determinism(rysmith)
  print("=== rysmith --n-params ===")
  test_n_params(rysmith)
  print("=== rysmith descriptor schema ===")
  test_descriptor_schema(rysmith)
  print("=== rysmith SOLVED header replay via symiri ===")
  test_solved_replay(rysmith, symiri)
  print("=== rysmith --emit-main wrapper ===")
  test_rysmith_main(rysmith, symiri)
  print("=== R3: scalar/ptr/vec self-assign elimination ===")
  test_r3_no_self_assign(rysmith)
  print("=== R3: plain-copy elimination ===")
  test_r3_no_plain_copy(rysmith)
  print("=== R3: LHS does not appear in RHS ===")
  test_r3_no_lhs_in_rhs(rysmith)
  print("=== R3: baseline assignment volume sanity ===")
  test_r3_baseline_assignment_volume(rysmith)
  print("=== R3: parameter uses survive in complex RHS ===")
  test_r3_pa_param_uses_present(rysmith)

  passed = sum(1 for _, ok, _ in results if ok)
  total = len(results)
  print(f"\nSummary (rysmith_tests): {passed}/{total} passed.\n")
  sys.exit(0 if passed == total else 1)


if __name__ == "__main__":
  main()
