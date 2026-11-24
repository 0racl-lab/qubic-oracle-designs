#!/bin/bash
# Apply the two implementation patches to clean upstream checkouts and test them:
#   qubic/core            patches/qubic-core.patch      (EvmCallRead + UtxoTxRead interfaces, tests)
#   qubic/oracle-machine  patches/oracle-machine.patch  (read_evm_call + read_utxo_tx services)
#
# Steps: clone both repos at the commits the patches were made against, apply the patches, run
# contract-verify on the new headers (if installed), build and run the new core tests on Linux,
# and run both services' selftests against their mock nodes.
#
# Usage: check/implementation.sh
# Env:   GTEST_INCLUDE / GTEST_LIBS to use a specific GoogleTest build (default: system libgtest-dev)
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$PWD
WORK=$ROOT/.impl
CORE_REF=${CORE_REF:-v1.313.0}
OM_SHA=${OM_SHA:-72192da}

fetch() {  # fetch <url> <dir> <ref>
  rm -rf "$2"; git clone -q "$1" "$2"; git -C "$2" checkout -q "$3"
}

echo "== qubic/core @ $CORE_REF + patches/qubic-core.patch"
fetch https://github.com/qubic/core.git "$WORK/core" "$CORE_REF"
git -C "$WORK/core" apply --whitespace=nowarn "$ROOT/patches/qubic-core.patch"

# the headers in oracle_interfaces/ must be exactly what the patch adds to core
for f in EvmCallRead UtxoTxRead; do
  cmp -s "oracle_interfaces/$f.h" "$WORK/core/src/oracle_interfaces/$f.h" \
    || { echo "oracle_interfaces/$f.h differs from the patched core header"; exit 1; }
done

if command -v contractverify >/dev/null 2>&1; then
  for f in EvmCallRead UtxoTxRead; do
    printf '%s: ' "$f"; contractverify --oi "$WORK/core/src/oracle_interfaces/$f.h" | tail -1
  done
else
  echo "contract-verify not installed: skipped (CI runs it)"
fi

echo "== core tests for the new interfaces (the block added to test/oracle_engine.cpp, built on Linux)"
C=$WORK/core
{ cat check/core_tests_head.cpp
  sed -n '/^\/\/ BEGIN EvmCallRead \/ UtxoTxRead/,/^\/\/ END EvmCallRead \/ UtxoTxRead/p' "$C/test/oracle_engine.cpp"
} > "$WORK/core_tests.cpp"
${CXX:-g++} -std=c++20 -mavx2 -mrdrnd -w -I "$C" -I "$C/src" -I "$C/lib/platform_common" -I "$C/lib/platform_os" \
  ${GTEST_INCLUDE:+-I "$GTEST_INCLUDE"} "$WORK/core_tests.cpp" ${GTEST_LIBS:--lgtest_main -lgtest} -lpthread -o "$WORK/core_tests"
"$WORK/core_tests" --gtest_brief=1

echo "== qubic/oracle-machine @ $OM_SHA + patches/oracle-machine.patch"
fetch https://github.com/qubic/oracle-machine.git "$WORK/om" "$OM_SHA"
git -C "$WORK/om" apply --whitespace=nowarn "$ROOT/patches/oracle-machine.patch"

PIDS=()
cleanup() { for p in "${PIDS[@]:-}"; do kill "$p" 2>/dev/null || true; done; }
trap cleanup EXIT

O=$WORK/om/oracles
python3 "$O/read_evm_call/test/mock_evm_rpc.py" --port 18645 & PIDS+=($!)
python3 "$O/read_evm_call/test/mock_evm_rpc.py" --port 18646 & PIDS+=($!)
python3 "$O/read_evm_call/test/mock_evm_rpc.py" --port 18647 --tamper & PIDS+=($!)
python3 "$O/read_utxo_tx/test/mock_utxo_node.py" --port 18745 & PIDS+=($!)
python3 "$O/read_utxo_tx/test/mock_utxo_node.py" --port 18746 & PIDS+=($!)
python3 "$O/read_utxo_tx/test/mock_utxo_node.py" --port 18747 --tamper & PIDS+=($!)
python3 "$O/read_utxo_tx/test/mock_utxo_node.py" --port 18748 --lag 45 & PIDS+=($!)
sleep 1
printf -- '-- read_evm_call: '; python3 "$O/read_evm_call/test/selftest.py" 2>/dev/null | tail -1
printf -- '-- read_utxo_tx (mock Dogecoin nodes): '; python3 "$O/read_utxo_tx/test/selftest.py" 2>/dev/null | tail -1
echo "== all implementation checks passed"
