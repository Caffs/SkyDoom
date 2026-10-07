#!/usr/bin/env bash
set -euo pipefail

if command -v clang-format-22 &>/dev/null; then
    FMT=clang-format-22
else
    echo "warning: clang-format-22 not found, using $(clang-format --version); CI uses v22" >&2
    FMT=clang-format
fi

find src \( -name '*.cpp' -o -name '*.h' \) -print0 | xargs -0 "$FMT" -i
