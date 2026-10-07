#!/usr/bin/env bash
# package.sh — build (optional) and create a distributable zip for NexusMods/mod managers.
# Requires bash 4+ (mapfile), GNU grep (-P), and 7z.
# Auto-build (without --build-dir) requires Linux (uses release-linux preset).
# Git Bash on Windows is supported with --build-dir pointing at an existing build.
#
# Usage: ./scripts/package.sh [--build-dir <dir>] [version]
#   --build-dir  skip cmake build; search <dir> for the DLL instead of build/release-linux
#   version      optional, e.g. 1.0.0 (default: exact git tag or CMakeLists.txt version)

set -euo pipefail

build_dir=""
positional=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir)
            if [[ -z "${2:-}" ]]; then
                echo "Error: --build-dir requires a value"
                exit 1
            fi
            build_dir="$2"; shift 2 ;;
        --*) echo "Error: unknown option: $1"; exit 1 ;;
        *) positional+=("$1"); shift ;;
    esac
done

if [[ ${#positional[@]} -gt 1 ]]; then
    echo "Error: unexpected arguments: ${positional[*]:1}"
    exit 1
fi

if [[ ! -f "CMakeLists.txt" ]]; then
    echo "Error: must be run from the project root (e.g. ./scripts/package.sh)"
    exit 1
fi

if ! command -v 7z &>/dev/null; then
    echo "Error: 7z is not installed"
    exit 1
fi

if [[ ${#positional[@]} -gt 0 ]]; then
    version="${positional[0]}"
elif git_tag=$(git describe --tags --exact-match 2>/dev/null); then
    version="${git_tag#v}"
else
    version=$(grep -oP '^\s+VERSION\s+\K[0-9]+\.[0-9]+\.[0-9]+' CMakeLists.txt | head -1 || true)
    if [[ -z "$version" ]]; then
        echo "Error: could not determine version from CMakeLists.txt"
        exit 1
    fi
fi

if [[ ! "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+(-[a-zA-Z0-9.]+)?(\+[a-zA-Z0-9.]+)?$ ]]; then
    echo "Error: invalid version '${version}' — expected X.Y.Z or X.Y.Z-pre or X.Y.Z+build"
    exit 1
fi

if [[ -z "$build_dir" ]]; then
    echo "Building..."
    cmake --preset release-linux
    cmake --build --preset release-linux
    build_dir="build/release-linux"
fi

project_name=$(grep -oP 'PROPERTIES OUTPUT_NAME \K[\w-]+' CMakeLists.txt || true)
if [[ -z "$project_name" ]]; then
    echo "Error: could not determine DLL name from CMakeLists.txt (OUTPUT_NAME not found)"
    exit 1
fi

mapfile -t dlls < <(find "$build_dir" -maxdepth 2 -type f -name "${project_name}.dll")
if [[ ${#dlls[@]} -ne 1 ]]; then
    echo "Error: expected exactly 1 DLL in ${build_dir}, found ${#dlls[@]}"
    exit 1
fi
dll="${dlls[0]}"

name=$(basename "$dll" .dll)
zip_name="dist/${name}-${version}.zip"

mkdir -p dist

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

mkdir -p "$tmp/SKSE/Plugins"
cp "$dll" "$tmp/SKSE/Plugins/"
(cd "$tmp" && 7z a -tzip out.zip SKSE > /dev/null)
mv "$tmp/out.zip" "$zip_name"

echo ""
echo "Package: ${zip_name}"

if [[ -n "${GITHUB_OUTPUT:-}" ]]; then
    echo "zip=${zip_name}" >> "$GITHUB_OUTPUT"
fi
