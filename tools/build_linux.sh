#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."
if [ "$(uname -s)" != "Linux" ] || [ "$(uname -m)" != "x86_64" ]; then
    echo "This bundled target requires Linux x86_64." >&2
    exit 2
fi
for tool in cmake ninja git javac; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "Missing Linux build tool: $tool (requires GCC, CMake, Ninja, Git and JDK 21+)." >&2
        exit 1
    fi
done
if [ -z "${JAVA_HOME:-}" ]; then
    export JAVA_HOME=$(dirname "$(dirname "$(readlink -f "$(command -v javac)")")")
fi

cmake --preset linux-release "$@"
cmake --build --preset linux-release --parallel "${ADVMATCH_BUILD_JOBS:-2}"
ctest --preset linux-release
cmake --build --preset linux-release --target package

mkdir -p build/native-bundles/linux-amd64
cp build/linux-release/libadvmatch.so build/linux-release/libadvmatch_jni.so build/native-bundles/linux-amd64/
