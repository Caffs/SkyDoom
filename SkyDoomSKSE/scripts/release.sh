#!/usr/bin/env bash
# release.sh: automate the release flow in docs/RELEASING.md.
#
# Requires the GitHub CLI (gh), authenticated with push/merge access.
#
# Usage: ./scripts/release.sh <major|minor|patch|X.Y.Z>
#
# Bumps CMakeLists.txt/vcpkg.json on a chore/release-X.Y.Z branch, opens a
# PR (labeled ignore-for-release), waits for its checks, squash-merges it,
# then tags and pushes vX.Y.Z, which triggers release.yml to build, package,
# and publish the GitHub Release.

set -euo pipefail

USAGE="Usage: ./scripts/release.sh <major|minor|patch|X.Y.Z>"

if [[ ! -f "CMakeLists.txt" ]]; then
    echo "Error: must be run from the project root (e.g. ./scripts/release.sh)" >&2
    exit 1
fi

if [[ $# -ne 1 ]]; then
    echo "$USAGE" >&2
    exit 1
fi

if ! command -v gh &>/dev/null; then
    echo "Error: gh (GitHub CLI) is not installed" >&2
    exit 1
fi

if ! gh auth status &>/dev/null; then
    echo "Error: gh is not authenticated (run: gh auth login)" >&2
    exit 1
fi

if [[ -n "$(git status --porcelain)" ]]; then
    echo "Error: working tree is not clean" >&2
    exit 1
fi

CURRENT_BRANCH=$(git branch --show-current)
if [[ "$CURRENT_BRANCH" != "main" ]]; then
    echo "Error: must be run on main (currently on ${CURRENT_BRANCH})" >&2
    exit 1
fi

echo "Fetching main and tags..."
git fetch --quiet origin main --tags
if [[ "$(git rev-parse main)" != "$(git rev-parse origin/main)" ]]; then
    echo "Error: local main differs from origin/main; pull first" >&2
    exit 1
fi

LATEST_VERSION=$(git tag --list | grep -E '^v[0-9]+\.[0-9]+\.[0-9]+$' | sort -V | tail -1 | sed 's/^v//' || true)
CMAKE_VERSION=$(grep -oP '^\s+VERSION\s+\K[0-9]+\.[0-9]+\.[0-9]+' CMakeLists.txt | head -1 || true)
VCPKG_VERSION=$(grep -oP '"version-string":\s*"\K[^"]+' vcpkg.json | head -1 || true)

echo "Current version: ${LATEST_VERSION:-none} (latest release tag)"

if [[ -n "$LATEST_VERSION" && ("$CMAKE_VERSION" != "$LATEST_VERSION" || "$VCPKG_VERSION" != "$LATEST_VERSION") ]]; then
    echo "Error: CMakeLists.txt (${CMAKE_VERSION}) / vcpkg.json (${VCPKG_VERSION}) do not match the latest release tag v${LATEST_VERSION}" >&2
    exit 1
fi

case "$1" in
    major | minor | patch)
        if [[ -z "$LATEST_VERSION" ]]; then
            echo "Error: no vX.Y.Z release tags found to bump from; pass an explicit version instead" >&2
            exit 1
        fi
        IFS='.' read -r MAJOR MINOR PATCH <<<"$LATEST_VERSION"
        case "$1" in
            major) TARGET_VERSION="$((MAJOR + 1)).0.0" ;;
            minor) TARGET_VERSION="${MAJOR}.$((MINOR + 1)).0" ;;
            patch) TARGET_VERSION="${MAJOR}.${MINOR}.$((PATCH + 1))" ;;
        esac
        ;;
    [0-9]*)
        TARGET_VERSION="$1"
        ;;
    *)
        echo "Error: unknown argument: $1" >&2
        echo "$USAGE" >&2
        exit 1
        ;;
esac

if [[ ! "$TARGET_VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "Error: invalid version '${TARGET_VERSION}'; expected X.Y.Z (CMake's project VERSION accepts no suffix)" >&2
    exit 1
fi

if [[ -n "$LATEST_VERSION" && "$(printf '%s\n' "$LATEST_VERSION" "$TARGET_VERSION" | sort -V | tail -1)" != "$TARGET_VERSION" ]]; then
    echo "Error: ${TARGET_VERSION} is not newer than the latest release tag v${LATEST_VERSION}" >&2
    exit 1
fi

if git rev-parse -q --verify "refs/tags/v${TARGET_VERSION}" >/dev/null; then
    echo "Error: tag v${TARGET_VERSION} already exists" >&2
    exit 1
fi

if [[ "$CMAKE_VERSION" == "$TARGET_VERSION" && "$VCPKG_VERSION" == "$TARGET_VERSION" ]]; then
    echo "Error: CMakeLists.txt and vcpkg.json are already at ${TARGET_VERSION}" >&2
    exit 1
fi

if ! gh label list --limit 200 --json name --jq '.[].name' | grep -qx ignore-for-release; then
    echo "Creating the ignore-for-release label..."
    gh label create ignore-for-release --color ededed --description "Excluded from generated release notes" >/dev/null
fi

BRANCH="chore/release-${TARGET_VERSION}"
echo "Creating ${BRANCH}..."
git checkout -b "$BRANCH"

echo "Bumping version to ${TARGET_VERSION}..."
sed -i -E "s/^(\s*VERSION\s+)[0-9]+\.[0-9]+\.[0-9]+/\1${TARGET_VERSION}/" CMakeLists.txt
sed -i -E "s/(\"version-string\":[[:space:]]*\")[^\"]+(\")/\1${TARGET_VERSION}\2/" vcpkg.json

CMAKE_NEW=$(sed -nE 's/^[[:space:]]+VERSION[[:space:]]+([^[:space:])]+).*/\1/p' CMakeLists.txt | head -1 || true)
VCPKG_NEW=$(sed -nE 's/.*"version-string":[[:space:]]*"([^"]+)".*/\1/p' vcpkg.json | head -1 || true)
if [[ "$CMAKE_NEW" != "$TARGET_VERSION" || "$VCPKG_NEW" != "$TARGET_VERSION" ]]; then
    echo "Error: could not bump CMakeLists.txt and vcpkg.json to ${TARGET_VERSION} (branch ${BRANCH} is checked out)" >&2
    exit 1
fi

git add CMakeLists.txt vcpkg.json
git commit --quiet -m "chore(release): ${TARGET_VERSION}"

echo "Pushing ${BRANCH}..."
git push --quiet -u origin "$BRANCH"

echo "Opening PR..."
PR_URL=$(gh pr create --title "chore(release): ${TARGET_VERSION}" --body "")
echo "$PR_URL"
PR_NUMBER="${PR_URL##*/}"
gh pr edit "$PR_NUMBER" --add-label ignore-for-release >/dev/null

echo "Waiting for checks to register on PR #${PR_NUMBER}..."
sleep 5
if ! gh pr checks "$PR_NUMBER" --watch; then
    echo "Error: checks failed on PR #${PR_NUMBER} (${PR_URL}). Fix and push to ${BRANCH}, then merge manually." >&2
    exit 1
fi

echo "Merging PR #${PR_NUMBER}..."
gh pr merge "$PR_NUMBER" --squash --delete-branch

git checkout --quiet main
git pull --quiet --ff-only origin main

MERGED_VERSION=$(sed -nE 's/^[[:space:]]+VERSION[[:space:]]+([^[:space:])]+).*/\1/p' CMakeLists.txt | head -1 || true)
if [[ "$MERGED_VERSION" != "$TARGET_VERSION" ]]; then
    echo "Error: main's version (${MERGED_VERSION}) does not match the expected ${TARGET_VERSION} after merge" >&2
    exit 1
fi

echo "Tagging v${TARGET_VERSION}..."
git tag "v${TARGET_VERSION}"
git push --quiet origin "v${TARGET_VERSION}"

echo ""
echo "Tag v${TARGET_VERSION} pushed. release.yml will build, package, and publish the GitHub Release."
echo "Watch it with: gh run watch"
echo ""
echo "Once release.yml completes, nexus-upload.yml runs automatically. Approve its"
echo "deployment in the nexus environment. Trigger it manually only if it does not start:"
echo "  gh workflow run nexus-upload.yml -f version=${TARGET_VERSION}"
echo "  (if the Nexus page text changed) python3 scripts/generate-nexus-page.py"
