#!/usr/bin/env bash
# init.sh — run once after cloning this template to set up a new mod project.
#
# Usage: ./scripts/init.sh "AuthorName" "ModName"
#   AuthorName  Your name (e.g. "YourName")
#   ModName     PascalCase name for your mod (e.g. "BetterJumping")
#
# Run without arguments to be prompted interactively.

set -euo pipefail

if [[ ! -f "CMakeLists.txt" ]]; then
    echo "Error: must be run from the project root (e.g. ./scripts/init.sh)"
    exit 1
fi

if [[ $# -ge 2 ]]; then
    AUTHOR="$1"
    MOD_NAME="$2"
else
    read -rp "Author name (e.g. YourName): " AUTHOR
    read -rp "Mod name — PascalCase (e.g. BetterJumping): " MOD_NAME
fi
MOD_NAME_LOWER="${MOD_NAME,,}"
MOD_NAME_LOWER="${MOD_NAME_LOWER//_/-}"

# Validate vcpkg-compatible name (lowercase letters, digits, hyphens only;
# no trailing or consecutive hyphens)
if [[ ! "$MOD_NAME_LOWER" =~ ^[a-z0-9]+(-[a-z0-9]+)*$ ]]; then
    echo "Error: mod name '${MOD_NAME}' is not valid."
    echo "  After lowercasing it must match ^[a-z0-9]+(-[a-z0-9]+)*$"
    echo "  Use only letters, digits, and hyphens (e.g. BetterJumping)"
    exit 1
fi

PLACEHOLDER="ExampleMod"
PLACEHOLDER_LOWER="examplemod"
PLACEHOLDER_AUTHOR="Author"

# Escape |, &, \ in author for use as sed replacement string
AUTHOR_ESC=$(printf '%s' "$AUTHOR" | sed 's/[|&\]/\\&/g')

echo "Renaming placeholders..."

FILES=(
    CMakeLists.txt
    src/Plugin.cpp
    vcpkg.json
    README.md
)

for file in "${FILES[@]}"; do
    if [[ -f "$file" ]]; then
        sed -i \
            -e "s|${PLACEHOLDER_LOWER}|${MOD_NAME_LOWER}|g" \
            -e "s|${PLACEHOLDER}|${MOD_NAME}|g" \
            -e "s|${PLACEHOLDER_AUTHOR}|${AUTHOR_ESC}|g" \
            "$file"
    else
        echo "  warning: $file not found, skipping"
    fi
done

echo "Initialising submodules..."
git submodule update --init --recursive

echo "Bootstrapping vcpkg..."
if [[ -f "lib/vcpkg/bootstrap-vcpkg.sh" ]]; then
    bash lib/vcpkg/bootstrap-vcpkg.sh -disableMetrics
else
    echo "Error: lib/vcpkg/bootstrap-vcpkg.sh not found — did submodules init correctly?"
    exit 1
fi

echo "Setting up .env..."
if [[ ! -f ".env" ]]; then
    cp .env.example .env
    echo "Copied .env.example to .env"
    echo ""
    echo "Edit .env and set your Skyrim paths before building:"
    echo "  SKYRIM_MODS_FOLDER — path to your mod manager's staging folder"
    echo "  SKYRIM_FOLDER      — path to your Skyrim installation (fallback)"
else
    echo ".env already exists, skipping"
fi

echo ""
echo "Done. Project set up as '${MOD_NAME}' by '${AUTHOR}'"
echo ""
echo "Next steps:"
echo "  1. Edit .env with your Skyrim paths"
echo "  2. Run: ./scripts/build.sh"
