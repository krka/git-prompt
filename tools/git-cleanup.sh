#!/bin/bash
# Clean up Git submodule to pristine state

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
GIT_SUBMODULE="$REPO_ROOT/submodules/git"

if [ ! -d "$GIT_SUBMODULE" ]; then
    echo "Error: Git submodule not found at $GIT_SUBMODULE"
    exit 1
fi

echo "Cleaning Git submodule..."
cd "$GIT_SUBMODULE"
git reset --hard HEAD
git clean -fd
echo "✓ Git submodule cleaned"
