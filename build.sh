#!/usr/bin/env sh
set -eu

script_dir=$(CDPATH= cd "$(dirname "$0")" && pwd)
cd "$script_dir"

preset="${1:-release}"

case "$preset" in
    debug|release)
        ;;
    *)
        echo "Usage: ./build.sh [debug|release]" >&2
        exit 2
        ;;
esac

if ! command -v cmake >/dev/null 2>&1; then
    echo "CMake 3.20 or newer is required and must be available on PATH." >&2
    exit 1
fi

if ! command -v git >/dev/null 2>&1; then
    echo "Git is required to download the pinned dependencies and must be available on PATH." >&2
    exit 1
fi

if ! command -v ninja >/dev/null 2>&1; then
    echo "Ninja is required by the checked-in CMake presets and must be available on PATH." >&2
    exit 1
fi

cmake --preset "$preset"
cmake --build --preset "$preset"
ctest --preset "$preset"
