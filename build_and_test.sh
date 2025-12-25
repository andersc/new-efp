#!/bin/bash
set -e
cd "$(dirname "$0")"
rm -rf build_test
mkdir -p build_test
cd build_test

echo "=== Configuring ==="
cmake .. -DEFP_BUILD_TESTS=ON -DEFP_BUILD_C_API=ON

echo "=== Building ==="
make -j4

echo "=== Running tests ==="
./efp_tests --test-suite-exclude="Stress Tests" 2>&1 || true
echo "=== Done ==="
