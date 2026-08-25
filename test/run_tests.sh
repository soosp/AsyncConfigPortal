#!/usr/bin/env sh
# run_tests.sh — build and run the JSON parsing host tests.
#
# Works on Linux, macOS, WSL, Git-Bash and MSYS2. Needs a C++17 compiler
# (g++ or clang++) on PATH. No make required.
#
#   ./run_tests.sh        (you may need:  chmod +x run_tests.sh  first)

# Always run from this script's own directory, so double-clicking or calling
# it from elsewhere still finds the sources and the library headers.
cd "$(dirname "$0")" || exit 1

# Pick a compiler: prefer g++, fall back to clang++ or the generic c++.
if   command -v g++      >/dev/null 2>&1; then CXX=g++
elif command -v clang++  >/dev/null 2>&1; then CXX=clang++
elif command -v c++      >/dev/null 2>&1; then CXX=c++
else
  echo "ERROR: no C++ compiler found (need g++, clang++ or c++ on PATH)."
  exit 1
fi

# Headers may be in the project root (..) or under ../src — search both.
FLAGS="-std=c++17 -O0 -Wall -I. -I.. -I../src"

# Optional sanitized run:  ./run_tests.sh --san
#
# The parser reads uploaded, untrusted input, so this is the run that actually
# covers the decision to hand-write it instead of taking a dependency. Opt-in
# because the sanitizers need runtime support that is not present everywhere
# (notably MinGW, which is why run_tests.bat has no equivalent).
SUFFIX=""
LABEL="JSON regression suite"
case "${1:-}" in
  --san)
    FLAGS="$FLAGS -fsanitize=address,undefined -fno-omit-frame-pointer -g"
    SUFFIX="_san"
    LABEL="$LABEL (sanitized)"
    # ASan halts on the first error by default; UBSan does NOT — it prints and
    # carries on, leaving the exit code at 0. Without halt_on_error the suite
    # would report ALL TESTS PASSED with a runtime error sitting in stderr.
    ASAN_OPTIONS="abort_on_error=1"
    UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1"
    export ASAN_OPTIONS UBSAN_OPTIONS
    ;;
  "") ;;
  *) echo "usage: $0 [--san]" >&2; exit 2 ;;
esac

build_and_run() {
  src="$1"; out="$2"; label="$3"
  echo "------------------------------------------------------------"
  echo "Building $label ..."
  if ! $CXX $FLAGS "$src" -o "$out"; then
    echo "BUILD FAILED: $src"
    return 1
  fi
  echo "Running $label:"
  "./$out"
  return $?
}

rc=0
build_and_run json_harness.cpp "json_harness${SUFFIX}" "$LABEL" || rc=1

echo "------------------------------------------------------------"
if [ "$rc" -eq 0 ]; then
  echo "ALL TESTS PASSED."
else
  echo "SOME TESTS FAILED (see output above)."
fi
exit "$rc"
