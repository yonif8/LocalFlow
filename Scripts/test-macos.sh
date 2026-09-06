#!/bin/bash
# Build the app and run every test in one SwiftPM plan.
set -euo pipefail
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"
source "$REPO_ROOT/Scripts/lib/macos-build.sh"
SCRATCH_DIR="$REPO_ROOT/.build"
if [[ $# == 2 && "$1" == --scratch-path ]]; then
    SCRATCH_DIR="$2"
elif [[ $# != 0 ]]; then
    echo "usage: Scripts/test-macos.sh [--scratch-path DIRECTORY]" >&2
    exit 2
fi
lf_macos_preflight
lf_build_and_test_macos "$SCRATCH_DIR"
