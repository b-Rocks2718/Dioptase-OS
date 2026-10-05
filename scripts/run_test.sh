#!/bin/sh
# Run one kernel test image repeatedly in the emulator and classify each run.
#
# Usage: run_test.sh MODE NAME RUNS TIMEOUT_SECONDS -- EMULATOR_ARGV...
#
# MODE selects reporting:
#   verbose   print one line per run, then "[NAME] summary: P/R"
#   failfast  like verbose, but stop at the first failing run
#   summary   print only "[NAME] pass: R/R" or "[NAME] fail: P/R (reasons)"
#
# Per run, emulator stdout goes to tests/NAME.raw and its "***" lines to
# tests/NAME.out. A run passes when:
#   - tests/NAME.panic exists: the output contains "PANIC", no "Warning" or
#     "Spurious", and every non-empty line of the .panic file as a substring
#     (tests/NAME.out then holds the PANIC lines);
#   - otherwise: the emulator exits 0 with no "Warning", "Spurious", or "PANIC",
#     and tests/NAME.out matches tests/NAME.ok byte for byte.
# A run killed by the timeout (exit 124) always fails.
#
# A test whose tests/NAME.requires does not match the configuration in the
# environment (OS_RELEASE, HEAP_DEBUG, OPT, NUM_CORES) prints
# "[NAME] skip: requires ..." without running.
#
# Exits 0 after reporting results on stdout, so aggregate make targets keep
# running every test. A malformed tests/NAME.requires is a harness error and
# exits 2 so make (and CI) fail loudly instead of silently dropping the test.

mode=$1
name=$2
runs=$3
timeout_seconds=$4
shift 4
if [ "$1" != "--" ]; then
  echo "run_test.sh: expected '--' before the emulator command, got '$1'" >&2
  exit 2
fi
shift

# A test whose tests/NAME.requires is not met by this configuration (passed
# in the environment; see test_requirements.sh) is reported as skipped.
script_dir=$(dirname "$0")
skip_reason=$(sh "$script_dir/test_requirements.sh" check "$name")
case $? in
  0) ;;
  1) echo "[$name] skip: $skip_reason"; exit 0 ;;
  *) echo "run_test.sh: tests/$name.requires is malformed; fix it before running $name." >&2; exit 2 ;;
esac

raw="tests/$name.raw"
out="tests/$name.out"
ok="tests/$name.ok"
panic_ok="tests/$name.panic"

# Classify the run whose output is in $raw and exit status in $status.
# Sets $result to pass, timeout, warning, missing_panic, panic_mismatch,
# exit, or mismatch.
classify_run() {
  if [ "$status" -eq 124 ]; then
    result=timeout
  elif [ -f "$panic_ok" ]; then
    grep 'PANIC' "$raw" > "$out" || true
    if grep -q "Warning" "$raw" || grep -q "Spurious" "$raw"; then
      result=warning
    elif ! grep -q "PANIC" "$raw"; then
      result=missing_panic
    else
      result=pass
      while IFS= read -r expected || [ -n "$expected" ]; do
        if [ -n "$expected" ] && ! grep -F -- "$expected" "$raw" > /dev/null; then
          result=panic_mismatch
        fi
      done < "$panic_ok"
    fi
  elif grep -q "Warning" "$raw" || grep -q "Spurious" "$raw" || grep -q "PANIC" "$raw"; then
    result=warning
  elif [ "$status" -ne 0 ]; then
    result=exit
  elif [ -f "$ok" ] && cmp -s "$out" "$ok"; then
    result=pass
  else
    result=mismatch
  fi
}

# Human-readable per-run description used by verbose and failfast modes.
describe_result() {
  case "$result" in
    pass) echo "pass" ;;
    timeout) echo "fail (timeout)" ;;
    warning) echo "fail (warning)" ;;
    missing_panic) echo "fail (missing panic)" ;;
    panic_mismatch) echo "fail (panic mismatch)" ;;
    exit) echo "fail (exit $status)" ;;
    *) echo "fail" ;;
  esac
}

success=0
timeout_failures=0
warning_failures=0
exit_failures=0
mismatch_failures=0

i=1
while [ "$i" -le "$runs" ]; do
  rm -f "$raw" "$out"
  status=0
  timeout "$timeout_seconds" "$@" > "$raw" || status=$?
  grep '^\*\*\*' "$raw" > "$out" || true
  classify_run

  case "$result" in
    pass) success=$((success + 1)) ;;
    timeout) timeout_failures=$((timeout_failures + 1)) ;;
    warning) warning_failures=$((warning_failures + 1)) ;;
    exit) exit_failures=$((exit_failures + 1)) ;;
    *) mismatch_failures=$((mismatch_failures + 1)) ;;
  esac

  if [ "$mode" != summary ]; then
    echo "[$name] run $i/$runs: $(describe_result)"
    if [ "$mode" = failfast ] && [ "$result" != pass ]; then
      break
    fi
  fi
  i=$((i + 1))
done

if [ "$mode" != summary ]; then
  echo "[$name] summary: $success/$runs"
elif [ "$success" -eq "$runs" ]; then
  echo "[$name] pass: $success/$runs"
else
  reasons=""
  if [ "$timeout_failures" -gt 0 ]; then reasons="$reasons $timeout_failures timeout"; fi
  if [ "$warning_failures" -gt 0 ]; then reasons="$reasons $warning_failures warning"; fi
  if [ "$exit_failures" -gt 0 ]; then reasons="$reasons $exit_failures exit"; fi
  if [ "$mismatch_failures" -gt 0 ]; then reasons="$reasons $mismatch_failures mismatch"; fi
  echo "[$name] fail: $success/$runs (${reasons# })"
fi
exit 0
