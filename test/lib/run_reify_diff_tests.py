"""Differential test: rysmith-generated programs interpreted by symiri vs.
compiled by symirc + gcc and run under UBSan. Catches bugs in any of
reify / interp / C-backend / solver by cross-checking their outputs.

Usage:
  python -m test.lib.run_reify_diff_tests \\
      --rysmith ./rysmith --symiri ./symiri --symirc ./symirc \\
      [--n 100] [--seed 1234] [--out build/test_tmp/reify_diff]
"""

import argparse
import os
import re
import shutil
import subprocess
import sys

from test.lib.style import bold, green, red, yellow

_FUN_RE = re.compile(r"fun\s+@([a-zA-Z0-9_]+)\(\)\s*:\s*(i[0-9]+|f32|f64)")
_RESULT_RE = re.compile(r"Result:\s*(-?[0-9]+)")


_CRET = {
  "i8": "int8_t",
  "i16": "int16_t",
  "i32": "int32_t",
  "i64": "int64_t",
}

# Batch size for generation + cross-validation. Large -n runs proceed
# one batch at a time so the user sees periodic progress (and so a
# 10000-program run does not need to materialise 10000 .sir/.c files
# on disk before testing starts). Each batch uses seed = base + batch_idx
# so generated programs do not repeat across batches.
BATCH_SIZE = 100


def _parse_fun(sir_path):
  """Returns (func_name, ret_type) for the first function declared, or None."""
  with open(sir_path, "r") as f:
    for line in f:
      m = _FUN_RE.search(line)
      if m:
        return m.group(1), m.group(2)
  return None


def _write_main_c(path, fname, ret_type):
  cret = _CRET.get(ret_type)
  if cret is None:
    return False  # f32/f64 returns: skip (need printf %f and tolerance)
  with open(path, "w") as f:
    f.write(
      "#include <stdint.h>\n"
      "#include <stdio.h>\n"
      f"extern {cret} symir_{fname}(void);\n"
      "int main(void) {\n"
      f"  {cret} r = symir_{fname}();\n"
      '  printf("Result: %lld\\n", (long long)r);\n'
      "  return 0;\n"
      "}\n"
    )
  return True


def _classify_one(sir_name, out_dir, symiri, clang, main_c, exe, verbose):
  """Run one (sir, c) pair and return (bucket, msg).

  ``bucket`` is one of: "passed", "skipped" (counter buckets, msg=None);
  or "mismatch", "ubsan", "cfail", "sirfail" (failure-list buckets, msg=str).
  """
  sir_path = os.path.join(out_dir, sir_name)
  c_path = sir_path[:-4] + ".c"
  if not os.path.exists(c_path):
    return ("cfail", f"{sir_name}: no .c emitted")

  fun = _parse_fun(sir_path)
  if fun is None:
    return ("sirfail", f"{sir_name}: cannot parse fun decl")
  fname, rtype = fun

  si = subprocess.run(
    [symiri, "--main", f"@{fname}", sir_path],
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
    text=True,
    timeout=10,
  )
  if si.returncode != 0:
    return ("sirfail", f"{sir_name}: symiri exit {si.returncode}")
  sm = _RESULT_RE.search(si.stdout) or _RESULT_RE.search(si.stderr)
  if sm is None:
    return ("sirfail", f"{sir_name}: symiri no Result line")
  sir_val = sm.group(1)

  if not _write_main_c(main_c, fname, rtype):
    return ("skipped", None)
  cc = subprocess.run(
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
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
    text=True,
    timeout=15,
  )
  if cc.returncode != 0:
    if verbose:
      print(cc.stderr)
    return ("cfail", f"{sir_name}: clang failed")
  cr = subprocess.run(
    [exe], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=5
  )
  if cr.returncode != 0:
    head = cr.stderr.strip().splitlines()[0] if cr.stderr else "trap"
    return ("ubsan", f"{sir_name}: {head}")
  cm = _RESULT_RE.search(cr.stdout)
  if cm is None:
    return ("cfail", f"{sir_name}: no Result from C binary")
  c_val = cm.group(1)

  if sir_val == c_val:
    return ("passed", None)
  return ("mismatch", f"{sir_name}: symiri={sir_val} c={c_val}")


def _clear_batch_files(out_dir):
  """Remove .sir/.c artefacts from a prior batch, preserving the harness."""
  for f in os.listdir(out_dir):
    if f in ("_main.c", "_test"):
      continue
    if f.endswith(".sir") or f.endswith(".c"):
      os.remove(os.path.join(out_dir, f))


def _save_bug(bugs_dir, bucket, sir_name, src_dir, batch_num):
  """Copy the failing .sir (and its companion .c, if present) into
  ``<bugs_dir>/<bucket>/`` so the case can be replayed/reduced after the
  next batch clears ``src_dir``. Filenames are prefixed with ``b<N>_`` to
  avoid collisions when rysmith reuses names across batches."""
  dst_dir = os.path.join(bugs_dir, bucket)
  os.makedirs(dst_dir, exist_ok=True)
  stem = sir_name[:-4] if sir_name.endswith(".sir") else sir_name
  prefix = f"b{batch_num}_"
  for suffix in (".sir", ".c"):
    src = os.path.join(src_dir, stem + suffix)
    if os.path.exists(src):
      shutil.copy2(src, os.path.join(dst_dir, prefix + stem + suffix))


def _print_test_summary(passed, mismatch, ubsan, cfail, sirfail, skipped):
  """The legacy per-test-block report. Used per-funder --verbose, and
  always at the end of a run."""
  print(f"  {green('passed')}:        {passed}")
  print(f"  {red('mismatched')}:    {len(mismatch)}")
  print(f"  {red('ubsan traps')}:   {len(ubsan)}")
  print(f"  {red('compile fail')}:  {len(cfail)}")
  print(f"  {red('symiri fail')}:   {len(sirfail)}")
  print(f"  {yellow('skipped')}:       {skipped}")


def run(rysmith, symiri, symirc, n, seed, out_dir, clang, verbose, fail_early=False):
  os.makedirs(out_dir, exist_ok=True)
  # Wipe stale artefacts from a previous run, including a prior bugs/ tree.
  for f in os.listdir(out_dir):
    p = os.path.join(out_dir, f)
    if os.path.isdir(p):
      shutil.rmtree(p)
    else:
      os.remove(p)
  bugs_dir = os.path.join(out_dir, "bugs")
  os.makedirs(bugs_dir, exist_ok=True)

  passed = 0
  mismatch = []
  ubsan = []
  cfail = []
  sirfail = []
  skipped = 0
  _LISTS = {"mismatch": mismatch, "ubsan": ubsan, "cfail": cfail, "sirfail": sirfail}

  main_c = os.path.join(out_dir, "_main.c")
  exe = os.path.join(out_dir, "_test")

  generated_total = 0  # programs rysmith actually emitted (skips its own FAILs)
  gen_failed_total = 0  # rysmith [FAIL] lines across batches
  processed_total = 0  # programs we cross-validated this run

  stopped_early = False
  done = 0
  batch_idx = 0
  while done < n and not stopped_early:
    batch_n = min(BATCH_SIZE, n - done)
    batch_seed = seed + batch_idx
    _clear_batch_files(out_dir)

    gen_cmd = [
      rysmith,
      "-n",
      str(batch_n),
      "--target",
      "c",
      "--seed",
      str(batch_seed),
      "-o",
      out_dir,
    ]
    if verbose:
      print(bold(f"batch #{batch_idx + 1} generation ({done}/{n} programs):"))
      print(f"  running: {' '.join(gen_cmd)}")
    gen = subprocess.run(
      gen_cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True
    )
    batch_gen_failed = (gen.stderr or "").strip().count("[FAIL]")
    gen_failed_total += batch_gen_failed

    sirs = sorted(p for p in os.listdir(out_dir) if p.endswith(".sir"))
    generated_total += len(sirs)

    if verbose:
      print(f"  {green('succeeded')}:     {batch_n - batch_gen_failed}")
      print(f"  {yellow('failed')}:        {batch_gen_failed}")
      print(f"  {'generated'}:     {len(sirs)}")

    done += batch_n

    if verbose:
      print(bold(f"batch #{batch_idx} testing ({done}/{n} programs):"))
      print("  running: symiri vs clang+exe")

    for sir_name in sirs:
      bucket, msg = _classify_one(
        sir_name, out_dir, symiri, clang, main_c, exe, verbose
      )
      processed_total += 1
      if bucket == "passed":
        passed += 1
      elif bucket == "skipped":
        skipped += 1
      else:
        _LISTS[bucket].append(msg)
        _save_bug(bugs_dir, bucket, sir_name, out_dir, batch_idx + 1)
        if fail_early:
          print(
            f"  [batch {batch_idx + 1}, prog {sir_name}] "
            f"{red('first failure')} ({bucket}): {msg}",
            flush=True,
          )
          stopped_early = True
          break

    batch_idx += 1

    if not stopped_early:
      if verbose:
        _print_test_summary(passed, mismatch, ubsan, cfail, sirfail, skipped)
      else:
        print(
          f"  [batch #{batch_idx}: {done}/{n}] passed={passed} "
          f"mismatch={len(mismatch)} ubsan={len(ubsan)} "
          f"cfail={len(cfail)} sirfail={len(sirfail)} "
          f"skipped={skipped}",
          flush=True,
        )
      print()

  # Report.
  print()
  hdr = f"reify differential test (n={processed_total}, seed={seed}"
  if stopped_early:
    hdr += ", stopped early"
  hdr += "):"
  print(bold(hdr))
  if verbose:
    print(f"  {'generated'}:     {generated_total}")
    print(f"  {red('gen failed')}:    {gen_failed_total}")
    print("  ---")
  _print_test_summary(passed, mismatch, ubsan, cfail, sirfail, skipped)

  fail_lines = mismatch + ubsan + cfail + sirfail
  if fail_lines:
    print(bold("\nFailures:"))
    for line in fail_lines[:20]:
      print(f"  {line}")
    if len(fail_lines) > 20:
      print(f"  ... and {len(fail_lines) - 20} more")
    print(
      f"\n  {yellow('saved')} {len(fail_lines)} failing case(s) to "
      f"{bugs_dir}/<bucket>/  (.sir + .c)"
    )

  return len(fail_lines) == 0


def main():
  ap = argparse.ArgumentParser()
  ap.add_argument("--rysmith", default="./rysmith")
  ap.add_argument("--symiri", default="./symiri")
  ap.add_argument(
    "--symirc",
    default="./symirc",
    help="(implicitly invoked by rysmith --target c)",
  )
  ap.add_argument("--clang", default="clang")
  ap.add_argument("--n", type=int, default=100)
  ap.add_argument(
    "--seed",
    type=int,
    default=1234,
    help="Fixed seed so make test is deterministic",
  )
  ap.add_argument("--out", default="build/test_tmp/reify_diff")
  ap.add_argument("--verbose", "-v", action="store_true")
  ap.add_argument(
    "--fail-early",
    action="store_true",
    help="Stop at the first mismatch/ubsan/cfail/sirfail instead of finishing the batch",
  )
  args = ap.parse_args()

  for tool, path in (
    ("rysmith", args.rysmith),
    ("symiri", args.symiri),
    ("symirc", args.symirc),
  ):
    if not os.path.exists(path):
      print(red(f"error: {tool} not found at {path}"), file=sys.stderr)
      sys.exit(2)
  if not shutil.which(args.clang):
    print(red(f"error: clang '{args.clang}' not on PATH"), file=sys.stderr)
    sys.exit(2)

  ok = run(
    args.rysmith,
    args.symiri,
    args.symirc,
    args.n,
    args.seed,
    args.out,
    args.clang,
    args.verbose,
    fail_early=args.fail_early,
  )
  sys.exit(0 if ok else 1)


if __name__ == "__main__":
  main()
