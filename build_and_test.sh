#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
build_dir="${script_dir}/build_test"

rm -rf "${build_dir}"

echo "=== Configuring ==="
cmake -S "${script_dir}" -B "${build_dir}" \
    -DEFP_BUILD_TESTS=ON \
    -DEFP_BUILD_C_API=ON

echo "=== Building ==="
cmake --build "${build_dir}" --parallel 4

echo "=== Running tests ==="
ctest --test-dir "${build_dir}" --output-on-failure

echo "=== Done ==="
