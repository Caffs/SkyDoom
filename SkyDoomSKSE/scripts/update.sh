#!/usr/bin/env bash
# update.sh: pin the CommonLibSSE-NG submodule to a release tag.
#
# Usage: ./scripts/update.sh [tag]    (default: the latest v* release tag)

set -euo pipefail

if [[ ! -f "CMakeLists.txt" ]]; then
	echo "Error: must be run from the project root (e.g. ./scripts/update.sh)"
	exit 1
fi

SUBMODULE="lib/commonlibsse-ng"

if [[ ! -e "${SUBMODULE}/.git" ]]; then
	echo "Error: ${SUBMODULE} is not initialised (run: git submodule update --init)"
	exit 1
fi

echo "Fetching CommonLibSSE-NG tags..."
git -C "${SUBMODULE}" fetch --tags origin ng

TAG="${1:-$(git -C "${SUBMODULE}" for-each-ref --sort=-v:refname --merged origin/ng --format='%(refname:short)' 'refs/tags/v[0-9]*' | grep -E '^v[0-9]+\.[0-9]+\.[0-9]+$' | head -n1 || true)}"
if [[ -z "${TAG}" ]]; then
	echo "Error: no release tag found on origin/ng in ${SUBMODULE}"
	exit 1
fi
if ! git -C "${SUBMODULE}" rev-parse --verify --quiet "refs/tags/${TAG}" >/dev/null; then
	echo "Error: tag '${TAG}' not found in ${SUBMODULE}"
	exit 1
fi

echo "Checking out CommonLibSSE-NG ${TAG}..."
git -C "${SUBMODULE}" checkout --quiet "refs/tags/${TAG}"
git -C "${SUBMODULE}" submodule update --init

git add "${SUBMODULE}"

echo ""
echo "Submodule pinned to ${TAG} and staged. Next step:"
echo "  git commit -m 'chore(deps): update CommonLibSSE-NG to ${TAG}'"
