#!/bin/sh
# Builds and runs the host tests from any directory.
set -eu
cd "$(dirname "$0")/../.."
mkdir -p build
c++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined \
  -DFIXTURES='"tests/fixtures/frames.txt"' \
  tests/host/test_core.cpp -o build/test_core
./build/test_core
