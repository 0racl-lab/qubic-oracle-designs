#!/bin/bash
# Check the FeedRegistry contract sketch (contracts/FeedRegistry.h):
#   1. type-check it against core's real QPI headers (check/feed_registry_core.cpp);
#   2. build and run its native tests (contracts/test/, QPI simulated by mock_qpi.h);
#   3. if Qubic's contract-verify tool is installed, run its contract rules on it;
#   4. with MUTATIONS=1, run the mutation check (every listed bug must make a test fail).
#
# Usage: check/contract.sh             (clones qubic/core into .core the first time)
# Env:   CORE=/path/to/core, MUTATIONS=1,
#        GTEST_INCLUDE / GTEST_LIBS to use a specific GoogleTest build (default: system libgtest-dev)
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$PWD
CORE="${CORE:-.core}"
if [ ! -d "$CORE/src/qpi" ]; then
  echo "Cloning qubic/core into $CORE ..."
  git clone --depth 1 https://github.com/qubic/core.git "$CORE"
fi

echo "== Type-checking against $(git -C "$CORE" log -1 --format='qubic/core %h (%cs)' 2>/dev/null || echo "$CORE")"
${CXX:-g++} -std=c++20 -fsyntax-only -mavx2 -w \
  -I "$CORE" -I "$CORE/src" -I "$CORE/lib/platform_common" -I "$CORE/lib/platform_os" \
  -I contracts check/feed_registry_core.cpp
echo "ok"

echo "== Native tests"
mkdir -p .contract
BUILD="${CXX:-g++} -std=c++20 -O0 -w -I \"\$FEEDREG_DIR\" ${GTEST_INCLUDE:+-I \"$GTEST_INCLUDE\"} \"$ROOT/contracts/test/feed_registry_test.cpp\" ${GTEST_LIBS:--lgtest_main -lgtest} -lpthread -o \"\$OUT\""
FEEDREG_DIR="$ROOT/contracts" OUT="$ROOT/.contract/feed_registry_test" bash -c "$BUILD"
.contract/feed_registry_test --gtest_brief=1

if command -v contractverify >/dev/null 2>&1; then
  echo "== contract-verify"
  contractverify contracts/FeedRegistry.h | tail -1
else
  echo "== contract-verify not installed: skipped (CI runs it; see .github/workflows/check.yml)"
fi

if [ "${MUTATIONS:-0}" = "1" ]; then
  echo "== Mutation check"
  python3 contracts/test/mutations.py "$BUILD" | grep -v '\] caught'
fi
