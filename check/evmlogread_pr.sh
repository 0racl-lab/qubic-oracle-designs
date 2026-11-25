#!/bin/bash
# Check the EvmLogRead block-time pull requests (prs/evmlogread-block-time/) on fresh clones:
#   qubic/core @ develop        + qubic-core.patch      -> contract-verify (if installed), EvmLogRead tests on Linux
#   qubic/oracle-machine @ main + oracle-machine.patch  -> read_evm_log selftest against its mock nodes
#
# Usage: check/evmlogread_pr.sh
# Env:   CORE_REF (default develop), OM_REF (default main),
#        GTEST_INCLUDE / GTEST_LIBS to use a specific GoogleTest build (default: system libgtest-dev)
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$PWD
WORK=$ROOT/.pr
P=$ROOT/prs/evmlogread-block-time
rm -rf "$WORK"; mkdir -p "$WORK"

echo "== qubic/core @ ${CORE_REF:-develop} + qubic-core.patch"
git clone -q --depth 1 --branch "${CORE_REF:-develop}" https://github.com/qubic/core.git "$WORK/core"
git -C "$WORK/core" log -1 --format='   base %h (%cs)'
git -C "$WORK/core" apply --whitespace=nowarn "$P/qubic-core.patch"
if command -v contractverify >/dev/null 2>&1; then
  printf 'EvmLogRead: '; contractverify --oi "$WORK/core/src/oracle_interfaces/EvmLogRead.h" | tail -1
fi
C=$WORK/core
{ cat check/core_tests_head.cpp
  sed -n '/^TEST(EvmLogReadOracle, DataContract)/,/^\/\/ --- QubicLogRead/p' "$C/test/oracle_engine.cpp"
} > "$WORK/evmlog_tests.cpp"
${CXX:-g++} -std=c++20 -mavx2 -mrdrnd -w -I "$C" -I "$C/src" -I "$C/lib/platform_common" -I "$C/lib/platform_os" \
  ${GTEST_INCLUDE:+-I "$GTEST_INCLUDE"} "$WORK/evmlog_tests.cpp" ${GTEST_LIBS:--lgtest_main -lgtest} -lpthread -o "$WORK/evmlog_tests"
"$WORK/evmlog_tests" --gtest_brief=1

echo "== qubic/oracle-machine @ ${OM_REF:-main} + oracle-machine.patch"
git clone -q --depth 1 --branch "${OM_REF:-main}" https://github.com/qubic/oracle-machine.git "$WORK/om"
git -C "$WORK/om" log -1 --format='   base %h (%cs)'
git -C "$WORK/om" apply --whitespace=nowarn "$P/oracle-machine.patch"
T=$WORK/om/oracles/read_evm_log/test
PIDS=()
cleanup() { for p in "${PIDS[@]:-}"; do kill "$p" 2>/dev/null || true; done; }
trap cleanup EXIT
python3 "$T/mock_evm_rpc.py" --port 18545 & PIDS+=($!)
python3 "$T/mock_evm_rpc.py" --port 18546 & PIDS+=($!)
python3 "$T/mock_evm_rpc.py" --port 18547 --tamper & PIDS+=($!)
python3 "$T/mock_evm_rpc.py" --port 18548 --tamper-time & PIDS+=($!)
sleep 1
printf -- '-- read_evm_log: '; python3 "$T/selftest.py" 2>/dev/null | tail -1
echo "== EvmLogRead block-time PR checks passed"
