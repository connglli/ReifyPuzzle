import difflib
import os
import sys

from test.lib.framework import TestResult, run_command, run_test_suite

# Maps EXPECT: FAIL:<subtype> to the exit code the tool must return.
FAIL_EXIT_CODES = {
  "FAIL:LexError": 2,
  "FAIL:ParseError": 3,
  "FAIL:StaticError": 4,
}


def run_analysis_test(binary_cmd_parts):
  def test_func(file_path, expectation, args, skips):
    if "ALL" in skips:
      return TestResult.SKIP, "Skipped by ALL tag (library file)"
    if "ANALYSIS" in skips:
      return TestResult.SKIP, "Skipped by ANALYSIS tag"

    cmd = binary_cmd_parts + [file_path] + args["COMPILER_ARGS"]
    result, err = run_command(cmd, timeout=5)

    if err == "TIMEOUT":
      return TestResult.TIMEOUT, "Timeout"

    passed = False
    if expectation == "PASS":
      passed = result.returncode == 0
    elif expectation == "FAIL":
      passed = result.returncode != 0
    elif expectation in FAIL_EXIT_CODES:
      passed = result.returncode == FAIL_EXIT_CODES[expectation]
    else:
      passed = result.returncode != 0  # unknown subtype: any failure

    if not passed:
      return (
        TestResult.FAIL,
        f"exit code {result.returncode} (expected {expectation})\n"
        f"STDOUT:\n{result.stdout}\nSTDERR:\n{result.stderr}",
      )

    # A sibling `<name>.sir.expected` pins the analysis dump: diff it
    # against stdout so structural regressions (not just exit codes)
    # are caught.
    expected_path = file_path + ".expected"
    if os.path.exists(expected_path):
      with open(expected_path, "r") as f:
        expected = f.read()
      if result.stdout != expected:
        diff = "".join(
          difflib.unified_diff(
            expected.splitlines(keepends=True),
            result.stdout.splitlines(keepends=True),
            fromfile=expected_path,
            tofile="stdout",
          )
        )
        return TestResult.FAIL, f"stdout does not match {expected_path}:\n{diff}"

    return TestResult.PASS, ""

  return test_func


if __name__ == "__main__":
  if len(sys.argv) < 3:
    print(
      "Usage: python3 -m test.lib.run_analysis_tests <test_dir> <symirc_path> [args...]"
    )
    sys.exit(1)

  test_dir = sys.argv[1]
  binary_cmd_parts = sys.argv[2:]

  run_test_suite("analysis_tests", test_dir, run_analysis_test(binary_cmd_parts))
