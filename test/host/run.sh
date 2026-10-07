#!/usr/bin/env sh
# Build and run the host-side unit tests (pure C++ parts of the firmware).
set -eu
cd "$(dirname "$0")"
out="${TMPDIR:-/tmp}/atomecho-host-tests"
mkdir -p "$out"
for t in test_*.cpp; do
    c++ -std=c++17 -O1 -Wall -Wextra -Werror -I../../src -o "$out/${t%.cpp}" "$t" -lm
    "$out/${t%.cpp}"
done
