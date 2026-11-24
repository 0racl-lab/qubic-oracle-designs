#!/bin/sh
# Check the interface sketches the way qubic/core would:
#   1. compile them against core's own QPI headers, next to the real interfaces, and check every
#      size and offset documented in the headers (check/sizes.cpp);
#   2. if Qubic's contract-verify tool is installed, run its oracle-interface check on each header.
#
# Usage: check/check.sh            (clones qubic/core into .core the first time)
#        CORE=/path/to/core check/check.sh
set -e
cd "$(dirname "$0")/.."
CORE="${CORE:-.core}"
if [ ! -d "$CORE/src/qpi" ]; then
  echo "Cloning qubic/core into $CORE ..."
  git clone --depth 1 https://github.com/qubic/core.git "$CORE"
fi

echo "== Compiling against $(git -C "$CORE" log -1 --format='qubic/core %h (%cs)' 2>/dev/null || echo "$CORE")"
${CXX:-g++} -std=c++20 -mavx2 -w \
  -I "$CORE" -I "$CORE/src" -I "$CORE/lib/platform_common" -I "$CORE/lib/platform_os" \
  -I oracle_interfaces check/sizes.cpp -o .sizes
./.sizes

if command -v contractverify >/dev/null 2>&1; then
  echo "== contract-verify (oracle interface rules)"
  for f in oracle_interfaces/*.h; do contractverify --oi "$f" | tail -1; done
else
  echo "== contract-verify not installed: skipped (CI runs it; see .github/workflows/check.yml)"
fi
