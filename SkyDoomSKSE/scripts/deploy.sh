#!/usr/bin/env bash
# deploy.sh — build and deploy the mod to your mod manager's staging folder.
#
# Requires SKYRIM_MODS_FOLDER to be set in .env or the environment.
#
# Usage: ./scripts/deploy.sh

set -euo pipefail

if [[ ! -f "CMakeLists.txt" ]]; then
    echo "Error: must be run from the project root (e.g. ./scripts/deploy.sh)"
    exit 1
fi

if [[ -f ".env" ]]; then
    # shellcheck source=/dev/null
    source .env
fi

if [[ -z "${SKYRIM_MODS_FOLDER:-}" ]]; then
    echo "Error: SKYRIM_MODS_FOLDER is not set."
    echo "  Set it in .env or export it before running this script."
    exit 1
fi

echo "Building and deploying to: ${SKYRIM_MODS_FOLDER}"
cmake --workflow --preset deploy

echo ""
echo "Deploy complete"
