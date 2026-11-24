# Qubic oracle designs

Proposed oracle interfaces for [Qubic](https://qubic.org). Two are implemented as ready-to-review patches for both repositories an oracle needs, **qubic/core** and **qubic/oracle-machine**. The other two are design notes with interface headers.

> **Status:** independent work, not part of Qubic and not yet reviewed by the core or oracle team. Nothing here is live on the network.

| Oracle | In one line | Status | Fee |
|---|---|---|---|
| [**EvmCallRead**](designs/evm-call-read.md) | Read EVM contract state (`eth_call`) at a finalized block | **Implemented:** core interface 5 + `read_evm_call` service | 1,000 QU |
| [**UtxoTxRead**](designs/utxo-tx-read.md) | Read one output of a confirmed **Dogecoin**, Bitcoin or Litecoin transaction | **Implemented:** core interface 6 + `read_utxo_tx` service | 1,000 QU |
| [**ContentRead**](designs/content-read.md) | Read a file by its SHA-256 hash | Design + header | 60–690 QU |
| [**ProofVerify**](designs/proof-verify.md) | Verify a zero-knowledge proof off-chain | Design + header | 500–750 QU |

## The implemented two

Each follows Qubic's existing EvmLogRead / `read_evm_log` pair exactly:

- **Interface conventions:** fixed-size query and reply, a `code` field, every other field zeroed on failure, time-varying failures marked retriable.
- **Service conventions:** stdlib-only Python, the same OM node framing, configuration by environment variable.
- **Safety:** multi-provider byte agreement, abstaining (committing nothing) on any disagreement or error.

**EvmCallRead** lets a contract read what something *is* on an EVM chain at a fixed, finalized block: a token balance, an NFT owner, a pool's reserves. EvmLogRead only proves that an event happened. The call is pinned to the block hash (EIP-1898), runs from the zero address with a fixed 10M gas cap, and returns a revert as a result rather than a failure.

**UtxoTxRead** lets a contract settle on a Dogecoin payment, and on Bitcoin and Litecoin payments too, since all three share a transaction format and node RPC. The answer comes only once the transaction is deep enough on every node the oracle machine uses (Dogecoin minimum 10 confirmations). Amounts are exact koinu, and scripts are returned raw.

### What the patches contain

| | qubic/core ([`patches/qubic-core.patch`](patches/qubic-core.patch)) | qubic/oracle-machine ([`patches/oracle-machine.patch`](patches/oracle-machine.patch)) |
|---|---|---|
| Interface / service | `src/oracle_interfaces/EvmCallRead.h`, `UtxoTxRead.h` | `oracles/read_evm_call/`, `oracles/read_utxo_tx/` |
| Registration | `oracle_core/oracle_interfaces_def.h` (indices 5, 6), `Qubic.vcxproj` | `om_common` config (slots 5, 6), `docker-compose.yml`, `example_env`, CMake install |
| Tests | 8 tests appended to `test/oracle_engine.cpp`, in the style of EvmLogRead's | `selftest.py` + mock nodes per service (29 and 33 checks), `live_check.py` for real endpoints |
| Based on | `v1.313.0` (2fee231) | `72192da` |

The headers in [`oracle_interfaces/`](oracle_interfaces) are byte-for-byte the ones the core patch adds; `check/implementation.sh` fails if they drift.

## Checks

```sh
check/implementation.sh    # clones both upstream repos, applies the patches, runs all the tests
check/check.sh             # compiles all four headers against qubic/core, checks sizes and offsets
```

`implementation.sh` does five things:

1. Clones qubic/core at `v1.313.0` and oracle-machine at `72192da`, and applies the patches.
2. Checks that the headers here match the patched core.
3. Runs contract-verify's oracle-interface rules on the new headers, if the tool is installed.
4. Builds and runs the 8 new core tests on Linux. Core's full suite is MSVC-only, but these tests only need QPI and the interface registry.
5. Runs both services' selftests against their mock nodes.

CI runs both scripts plus the official [contract-verify](https://github.com/qubic/contract-verify) action on every push.

Last local run:

```
EvmCallRead: Oracle interface compliance check PASSED
UtxoTxRead: Oracle interface compliance check PASSED
[  PASSED  ] 8 tests.
-- read_evm_call: SELFTEST PASSED (0 failure(s))
-- read_utxo_tx (mock Dogecoin nodes): SELFTEST PASSED (0 failure(s))
```

**Not tested yet:**

- **Real endpoints and nodes.** The selftests use mocks. `live_check.py` in each service runs one real query against your own endpoints: Base or Ethereum RPCs for EvmCallRead, your own dogecoind for UtxoTxRead.
- **Core's full MSVC suite on Windows.**
- **A full network round trip** through a Qubic node and an oracle machine on a testnet.

## How an oracle interface gets added to Qubic

Oracle interfaces don't go through a computor proposal or an IPO. Per core's [oracle guide](https://github.com/qubic/core/blob/main/doc/contracts_oracles.md), the steps are:

1. **Interface header** in `src/oracle_interfaces/`, registered in `src/oracle_core/oracle_interfaces_def.h`. That's the core patch.
2. **Reference service** in [oracle-machine](https://github.com/qubic/oracle-machine). That's the oracle-machine patch, which needs its core submodule to point at a core with the new interfaces.
3. **Pull requests** to both repositories, reviewed by core developers. The interface ships in a core release that computors choose to run.

## Repository layout

```
designs/              one design note per oracle: purpose, how it works, rules, limits, tests, open questions
oracle_interfaces/    the interface headers (drop-in format for qubic/core src/oracle_interfaces/)
patches/              git format-patch files for qubic/core and qubic/oracle-machine
check/                check.sh (headers vs core), implementation.sh (patches + all tests)
.github/workflows/    CI
```

## Contributing

Issues and pull requests are welcome, especially answers to the open questions at the end of each design note.

## Licence

[MIT](LICENSE). The patches modify qubic/core and qubic/oracle-machine; those changes are offered under each project's own licence when contributed upstream. This project is independent and not affiliated with or endorsed by Qubic.
