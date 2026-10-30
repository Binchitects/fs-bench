#!/usr/bin/env sh
# Builds fsbench on Linux. Uses CMake when available, otherwise calls the compiler directly.
set -eu
cd "$(dirname "$0")"

if command -v cmake >/dev/null 2>&1; then
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build -j"$(nproc 2>/dev/null || echo 4)"
    echo "built: build/fsbench"
else
    CXX="${CXX:-g++}"
    mkdir -p build
    # shellcheck disable=SC2046
    "$CXX" -std=c++20 -O2 -Wall -Wextra -pthread -Isrc \
        $(ls src/*.cpp src/tests/*.cpp src/bench/*.cpp 2>/dev/null) \
        -o build/fsbench
    echo "built: build/fsbench"
fi
