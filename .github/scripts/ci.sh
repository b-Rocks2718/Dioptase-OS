#!/usr/bin/env bash

set -euo pipefail

readonly REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
readonly WORKSPACE_DIR="$(cd "$REPO_DIR/.." && pwd)"
readonly ASSEMBLER_DIR="$WORKSPACE_DIR/Dioptase-Assembler"
readonly COMPILER_DIR="$WORKSPACE_DIR/Dioptase-Languages/Dioptase-C-Compiler"
readonly EMULATOR_DIR="$WORKSPACE_DIR/Dioptase-Emulators/Dioptase-Emulator-Full"
readonly OS_TEST_RUNS="${DIOPTASE_OS_TEST_RUNS:-5}"
readonly OS_PARALLEL_JOBS="${DIOPTASE_OS_PARALLEL_JOBS:-4}"
readonly EXCLUDED_STRESS_TEST="user_bcc_include_recursion"

for command in cargo cmp du gcc grep make mkfs.ext2 python3 sed tee timeout; do
  if ! command -v "$command" >/dev/null 2>&1; then
    echo "OS CI: required host command '$command' was not found in PATH." >&2
    exit 1
  fi
done

for value_name in OS_TEST_RUNS OS_PARALLEL_JOBS; do
  value="${!value_name}"
  if [[ ! "$value" =~ ^[1-9][0-9]*$ ]]; then
    echo "OS CI: $value_name must be a positive integer, observed '$value'." >&2
    exit 1
  fi
done

for dependency_dir in "$ASSEMBLER_DIR" "$COMPILER_DIR" "$EMULATOR_DIR"; do
  if [ ! -d "$dependency_dir" ]; then
    echo "OS CI: required sibling dependency directory '$dependency_dir' does not exist." >&2
    exit 1
  fi
done

make -C "$ASSEMBLER_DIR" release
cargo build --release --manifest-path "$EMULATOR_DIR/Cargo.toml"
make -C "$COMPILER_DIR" release
make -C "$REPO_DIR" clean

shopt -s nullglob
baselines=("$REPO_DIR"/tests/*.ok "$REPO_DIR"/tests/*.panic)
declare -A seen_tests=()
test_targets=()

for baseline in "${baselines[@]}"; do
  filename="${baseline##*/}"
  test_name="${filename%.*}"

  # This guest-self-hosting recursion test is intentionally a local stress
  # test: even one execution is much slower than the rest of the PR smoke suite.
  if [ "$test_name" = "$EXCLUDED_STRESS_TEST" ]; then
    continue
  fi

  if [ -z "${seen_tests[$test_name]+present}" ]; then
    seen_tests[$test_name]=1
    test_targets+=("$test_name.fail")
  fi
done

if [ "${#test_targets[@]}" -eq 0 ]; then
  echo "OS CI: no .ok or .panic baselines were found under '$REPO_DIR/tests'." >&2
  exit 1
fi

log="$(mktemp)"
trap 'rm -f "$log"' EXIT

set +e
make -C "$REPO_DIR" \
  -j"$OS_PARALLEL_JOBS" \
  --output-sync=target \
  VERSION=release \
  TEST_RUNS="$OS_TEST_RUNS" \
  "${test_targets[@]}" 2>&1 | tee "$log"
make_status=${PIPESTATUS[0]}
set -e

if [ "$make_status" -ne 0 ]; then
  echo "OS CI: test harness exited with status $make_status." >&2
  exit "$make_status"
fi

if grep -E '^\[[^]]+\] run [0-9]+/[0-9]+: fail' "$log"; then
  echo "OS CI: at least one baseline-checked test run failed." >&2
  exit 1
fi
