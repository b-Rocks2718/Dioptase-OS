#!/bin/sh
# Decide which tests can produce a meaningful verdict in the current build
# configuration.
#
# Most tests must pass in every configuration. A test whose expected result
# inherently depends on the configuration (for example, a .panic baseline that
# expects a HEAP_DEBUG-only check to fire) declares that in
# tests/<name>.requires, one requirement per line:
#
#   KEY=VALUE     the current value of KEY must equal VALUE
#   KEY!=VALUE    the current value of KEY must not equal VALUE
#   # comment     blank lines and '#' comments are ignored
#
# Known keys, read from the environment (the Makefile passes them):
#   OS_RELEASE  yes|no   soft kernel asserts compiled out
#   HEAP_DEBUG  yes|no   slab bitmap / poison checking
#   OPT         yes|no   bcc optimizations enabled (BCC_OPT non-empty)
#   NUM_CORES   number of emulated cores
#
# Usage:
#   test_requirements.sh unsupported   print each test whose requirements fail
#   test_requirements.sh check NAME    exit 0 if NAME is supported; otherwise
#                                      print the first unmet requirement and
#                                      exit 1
# A malformed line or unknown key is an error (exit 2) rather than a silent
# skip, so a typo cannot quietly drop a test from the suite.

tests_dir=tests

# Print the current value of a known configuration key.
config_value() {
  case "$1" in
    OS_RELEASE) printf '%s' "$OS_RELEASE" ;;
    HEAP_DEBUG) printf '%s' "$HEAP_DEBUG" ;;
    OPT) printf '%s' "$OPT" ;;
    NUM_CORES) printf '%s' "$NUM_CORES" ;;
    *) return 1 ;;
  esac
}

# Check every requirement in one .requires file. Prints the first unmet
# requirement and returns 1; returns 0 when all hold.
check_file() {
  file=$1
  while IFS= read -r line || [ -n "$line" ]; do
    line=$(printf '%s' "$line" | sed 's/#.*//; s/^[[:space:]]*//; s/[[:space:]]*$//')
    [ -z "$line" ] && continue

    case "$line" in
      *!=*) key=${line%%!=*}; want=${line#*!=}; negate=1 ;;
      *=*) key=${line%%=*}; want=${line#*=}; negate=0 ;;
      *)
        echo "test_requirements.sh: $file: expected KEY=VALUE or KEY!=VALUE, got '$line'" >&2
        exit 2 ;;
    esac

    if ! have=$(config_value "$key"); then
      echo "test_requirements.sh: $file: unknown key '$key' (expected OS_RELEASE, HEAP_DEBUG, OPT, or NUM_CORES)" >&2
      exit 2
    fi

    if [ "$negate" -eq 0 ] && [ "$have" != "$want" ]; then
      echo "requires $key=$want (have $key=$have)"
      return 1
    fi
    if [ "$negate" -eq 1 ] && [ "$have" = "$want" ]; then
      echo "requires $key!=$want (have $key=$have)"
      return 1
    fi
  done < "$file"
  return 0
}

case "$1" in
  unsupported)
    for file in "$tests_dir"/*.requires; do
      [ -f "$file" ] || continue
      if ! check_file "$file" > /dev/null; then
        name=${file##*/}
        echo "${name%.requires}"
      fi
    done
    ;;
  check)
    file="$tests_dir/$2.requires"
    [ -f "$file" ] || exit 0
    check_file "$file"
    ;;
  *)
    echo "usage: test_requirements.sh unsupported | check NAME" >&2
    exit 2
    ;;
esac
