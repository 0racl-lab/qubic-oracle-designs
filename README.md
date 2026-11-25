# Qubic oracle designs

Proposed oracle interfaces for [Qubic](https://qubic.org). Four are implemented as ready-to-review patches for both repositories an oracle needs, **qubic/core** and **qubic/oracle-machine**. A fifth, HttpRead, is a plan with a draft interface header for a safe "curl" oracle.

> **Status:** independent work, not part of Qubic and not yet reviewed by the core or oracle team. Nothing here is live on the network.

| Oracle | In one line | Status | Fee |
|---|---|---|---|
| [**EvmCallRead**](designs/evm-call-read.md) | Read EVM contract state (`eth_call`) at a finalized block | **Implemented:** core interface 5 + `read_evm_call` service | 1,000 QU |
| [**UtxoTxRead**](designs/utxo-tx-read.md) | Read one output of a confirmed **Dogecoin**, Bitcoin or Litecoin transaction | **Implemented:** core interface 6 + `read_utxo_tx` service | 1,000 QU |
| [**ContentRead**](designs/content-read.md) | Read a file by its SHA-256 hash: size, byte range or one JSON field | **Implemented:** core interface 7 + `read_content` service | 60–690 QU |
| [**EvmTxRead**](designs/evm-tx-read.md) | When and by whom an EVM transaction happened: block, timestamp, sender, value, status | **Implemented:** core interface 8 + `read_evm_tx` service | 1,000 QU |
| [EvmLogRead block time](prs/evmlogread-block-time/) | Pull request: add block number and timestamp to Qubic's existing EvmLogRead reply | **Ready to open:** core + oracle-machine patches | unchanged |
| [**HttpRead**](designs/http-read.md) | Read one value from an approved public web API | Plan + draft header | 200 QU |
| [**FeedRegistry**](designs/http-read.md#feeds-governed-by-a-registry-contract) | Web feeds voted in by shareholders and read through HttpRead | Contract sketch + 53 tests | Oracle fee + feed fee |

## How Qubic oracles work (what this builds on)

Two points from the oracle-machine developers shape every design here:

- **Quorum agreement, not truth.** The oracle machine has no concept of truth. Computors accept a reply when a quorum of their oracle machines produced the same bytes, which stops anyone manipulating the path between the source and the contract. If one source isn't reliable enough, the oracle machine can combine several: the Price oracle's `gate_mexc` source already averages gate.io and MEXC for QUBIC/USDT.
- **Contracts are the main users.** Oracles exist so contracts can actively query data from outside Qubic when their logic needs it. Queries sent by users are mainly for developers testing their work.

So each interface here says exactly what a quorum is agreeing on (a call result, a transaction, a transaction output, a file's bytes, a source's published value), and leaves judging that data to the contract.

## The implemented four

Each follows Qubic's existing EvmLogRead / `read_evm_log` pair exactly:

- **Interface conventions:** fixed-size query and reply, a `code` field, every other field zeroed on failure, time-varying failures marked retriable.
- **Service conventions:** stdlib-only Python, the same OM node framing, configuration by environment variable.
- **Safety:** multi-provider byte agreement, abstaining (committing nothing) on any disagreement or error.

**EvmCallRead** lets a contract read what something *is* on an EVM chain at a fixed, finalized block: a token balance, an NFT owner, a pool's reserves. EvmLogRead only proves that an event happened. The call is pinned to the block hash (EIP-1898), runs from the zero address with a fixed 10M gas cap, and returns a revert as a result rather than a failure.

**EvmTxRead** is EvmLogRead's sibling. EvmLogRead returns what a transaction emitted; EvmTxRead returns when and by whom: block number, block timestamp, sender, recipient, native value and status. With both, a contract can accept a plain ERC-20 transfer, including USDT, which has no signed-transfer support, only if it was made after an order was opened. A reverted transaction is a final answer (`status = 0`), not "not finalized".

**UtxoTxRead** lets a contract settle on a Dogecoin payment, and on Bitcoin and Litecoin payments too, since all three share a transaction format and node RPC. The answer comes only once the transaction is deep enough on every node the oracle machine uses (Dogecoin minimum 10 confirmations). Amounts are exact koinu, and scripts are returned raw.

**ContentRead** lets a contract keep only a file's SHA-256 on-chain and read the file when it needs to: its size, up to 512 bytes, or one JSON field. Files come through IPFS gateways, Arweave or mirrors, but only bytes that match the hash are ever used, so a lying gateway can't change an answer. JSON comes back in RFC 8785 canonical form, so every machine produces identical bytes.

## The plan for a curl-like oracle

[HttpRead](designs/http-read.md) explains why a literal "fetch any URL" oracle can't work on Qubic (machines must agree byte for byte, and the web doesn't), and what can:

- **Reviewed adapters per source**, not raw URLs.
- **Answers pinned** to a past timestamp or a final published figure.
- **Sources combined inside the oracle machine** where one provider isn't enough, like Price's `gate_mexc`.
- **One field out**, scaled exactly.
- **Abstain on any doubt.**

It covers launch candidates (ECB reference rates via Frankfurter, Open-Meteo's weather archive), who decides which sources exist, and a phased roadmap. The last phase lets feeds be voted in through a FeedRegistry contract, while the fetching stays in reviewed oracle-machine code. It makes no claim to truth: a reply is what a quorum of oracle machines agreed the source said.

### FeedRegistry: a contract sketch

[`contracts/FeedRegistry.h`](contracts/FeedRegistry.h) is that registry written as a QPI contract, assuming HttpRead is live:

- **Validation on-chain.** `https` only, a DNS host name (no IP addresses, ports or `user@`), URL-safe paths, typed placeholders, zero padding so each definition has one hash.
- **Lifecycle.** Proposing a feed takes a 1 billion QU deposit. Feeds go live by standard shareholder yes/no vote, which refunds the deposit. Unvoted proposals expire after 4 epochs and their deposit is burned. Feeds are retired by vote. Definitions never change.
- **The registry is the gateway.** Contracts call `RequestValue`; the registry checks the caller, the timestamp and every parameter, takes the fee and sends an HttpRead query for the `feedreg` source. Oracle machines only serve a `feedreg` query whose request the registry recorded with that query id, so nobody can skip the registry or its fee.
- **Fees.** A feed can charge per use and can be restricted to one contract. The proposer gets a share, shareholders get the rest, and the fee is refunded if the oracle doesn't answer.

It is a sketch for discussion, not a contract proposal. The [design note](designs/http-read.md#feeds-governed-by-a-registry-contract) lists what core has to agree first.

### What the patches contain

Each repository gets a series of three patches, applied in order: `0001` adds EvmCallRead and UtxoTxRead, `0002` adds ContentRead, `0003` adds EvmTxRead.

| | qubic/core ([`patches/qubic-core/`](patches/qubic-core)) | qubic/oracle-machine ([`patches/oracle-machine/`](patches/oracle-machine)) |
|---|---|---|
| Interface / service | `src/oracle_interfaces/EvmCallRead.h`, `UtxoTxRead.h`, `ContentRead.h`, `EvmTxRead.h` | `oracles/read_evm_call/`, `oracles/read_utxo_tx/`, `oracles/read_content/`, `oracles/read_evm_tx/` |
| Registration | `oracle_core/oracle_interfaces_def.h` (indices 5–8), `Qubic.vcxproj` | `om_common` config (slots 5–8), `docker-compose.yml`, `example_env`, CMake install |
| Tests | 17 tests appended to `test/oracle_engine.cpp`, in the style of EvmLogRead's | `selftest.py` + mocks per service (29, 33, 45 and 33 checks), `live_check.py` for real endpoints |
| Based on | `v1.313.0` (2fee231); also applies to today's `main` | `72192da` (current `main`) |

The headers in [`oracle_interfaces/`](oracle_interfaces) are byte-for-byte the ones the core patch adds; `check/implementation.sh` fails if they drift.

## Checks

```sh
check/implementation.sh    # clones both upstream repos, applies the patches, runs all the tests
check/check.sh             # compiles all five headers against qubic/core, checks sizes and offsets
check/contract.sh          # FeedRegistry: type-check against qubic/core, 53 native tests, contract-verify
MUTATIONS=1 check/contract.sh   # ... plus the mutation check (114 deliberate bugs, each must fail a test)
check/evmlogread_pr.sh     # EvmLogRead block-time PR: core develop + oracle-machine main, tests and selftest
```

`implementation.sh` does five things:

1. Clones qubic/core at `v1.313.0` and oracle-machine at `72192da`, and applies the patches.
2. Checks that the headers here match the patched core.
3. Runs contract-verify's oracle-interface rules on the new headers, if the tool is installed.
4. Builds and runs the 17 new core tests on Linux. Core's full suite is MSVC-only, but these tests only need QPI and the interface registry.
5. Runs all four services' selftests against their mock nodes.

`contract.sh` type-checks the contract against core's real QPI headers. The tests themselves run against `contracts/test/mock_qpi.h`, a small simulation of QPI: balances, burns, dividends, shareholder voting with core's 2/3 quorum rule, the hash map, dates and the HttpRead oracle (queries, fees and callbacks). Building a contract inside core needs MSVC.

CI runs all four scripts (with the mutation check) plus the official [contract-verify](https://github.com/qubic/contract-verify) action on every push.

Last local run:

```
EvmCallRead: Oracle interface compliance check PASSED
UtxoTxRead: Oracle interface compliance check PASSED
ContentRead: Oracle interface compliance check PASSED
EvmTxRead: Oracle interface compliance check PASSED
[  PASSED  ] 17 tests.
-- read_evm_call: SELFTEST PASSED (0 failure(s))
-- read_utxo_tx (mock Dogecoin nodes): SELFTEST PASSED (0 failure(s))
-- read_content (mock gateways): SELFTEST PASSED (0 failure(s))
-- read_evm_tx: SELFTEST PASSED (0 failure(s))
```

**Not tested yet:**

- **Real endpoints and nodes.** The selftests use mocks. `live_check.py` in each service runs one real query against your own endpoints: Base or Ethereum RPCs for EvmCallRead and EvmTxRead, your own dogecoind for UtxoTxRead, public IPFS gateways for ContentRead.
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
patches/              git format-patch series for qubic/core and qubic/oracle-machine (apply in order)
prs/                  stand-alone pull requests to existing Qubic code, with patches and PR descriptions
contracts/            FeedRegistry.h (contract sketch); test/ holds its native tests, QPI mock and mutation list
check/                check.sh (headers vs core), implementation.sh (patches + all tests), contract.sh (FeedRegistry)
.github/workflows/    CI
```

## Contributing

Issues and pull requests are welcome, especially answers to the open questions at the end of each design note.

## Licence

[MIT](LICENSE). The patches modify qubic/core and qubic/oracle-machine; those changes are offered under each project's own licence when contributed upstream. This project is independent and not affiliated with or endorsed by Qubic.
