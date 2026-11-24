# Qubic oracle designs

Four proposed oracle interfaces for [Qubic](https://qubic.org), each written as a design note plus a header file in the exact format `qubic/core` uses for its own oracle interfaces.

> **Status:** design sketches for discussion. They are not part of qubic/core and have not been reviewed by the core or oracle team. Nothing here is live on the network.

| Oracle | In one line | What it unlocks | Fee |
|---|---|---|---|
| [**ContentRead**](designs/content-read.md) | Read a file by its SHA-256 hash | Data registries, manifests, NFT metadata, storage markets | 60–690 QU |
| [**BtcTxRead**](designs/btc-tx-read.md) | Read one output of a confirmed Bitcoin transaction | QU ⇄ BTC trades and BTC payments with no bridge | 1,000 QU |
| [**EvmCallRead**](designs/evm-call-read.md) | Read EVM contract state at a finalized block | Token-gating, NFT ownership, collateral checks | 1,000 QU |
| [**ProofVerify**](designs/proof-verify.md) | Verify a zero-knowledge proof off-chain | zkTLS fiat payment proofs, Web2 account proofs, privacy checks | 500–750 QU |

## Why these four

Qubic's oracle machines each answer a query independently, and computors only accept a reply that a quorum agrees on byte for byte. So the easiest oracles to get right are the ones where **the right answer can't change**:

- **ContentRead:** the content hash fixes the bytes.
- **BtcTxRead:** a block buried under enough confirmations doesn't change.
- **EvmCallRead:** state at a finalized block is permanent.
- **ProofVerify:** verifying a proof is pure maths.

Each one follows the conventions of Qubic's existing interfaces (`Price`, `EvmLogRead`, `QubicLogRead`, `DogeShareValidation`, `Mock`):

- fixed-size query and reply structs within core's 1,008-byte limits;
- a `code` field, with every other field zeroed on failure so failed replies also match;
- time-varying failures marked as retriable.

## Repository layout

```
designs/              one design note per oracle: what it's for, how it works, rules, limits, open questions
oracle_interfaces/    the interface headers, drop-in format for qubic/core src/oracle_interfaces/
check/                compiles the headers against qubic/core and checks their sizes and offsets
.github/workflows/    CI: Qubic's contract-verify (oracle interface rules) + the compile check
```

## Checks

```sh
check/check.sh                         # clones qubic/core into .core the first time
CORE=/path/to/qubic/core check/check.sh
```

The check does two things:

- **Compile:** builds the four headers against core's own QPI headers, alongside the five real interfaces. It confirms each query and reply fits core's limits and has exactly the size and field offsets documented in the header.
- **Rules:** if Qubic's [contract-verify](https://github.com/qubic/contract-verify) tool is installed, it runs its oracle-interface rules (`contractverify --oi`) on each header. CI always runs both.

Current result, against qubic/core `2fee231` (v1.313.0):

```
ContentRead  query  208  reply  536  fee 60..690 QU
BtcTxRead    query   56  reply  208  fee 1000 QU
EvmCallRead  query  568  reply  568  fee 1000 QU
ProofVerify  query  808  reply  112  fee 750/500 QU (by hash/inline)
limits: query <= 1008, reply <= 1008 bytes
Oracle interface compliance check PASSED  (x4)
```

## How an oracle interface gets added to Qubic

Oracle interfaces don't go through a computor proposal or an IPO. Per core's [oracle guide](https://github.com/qubic/core/blob/main/doc/contracts_oracles.md), the steps are:

1. **Write the interface header** in `src/oracle_interfaces/`. That's what `oracle_interfaces/` here contains.
2. **Register it** in `src/oracle_core/oracle_interfaces_def.h`.
3. **Write the reference service** in the [oracle-machine](https://github.com/qubic/oracle-machine) repository. Oracle machines run this code to answer queries.
4. **Open pull requests.** Core developers review them, and the interface ships in a core release that computors choose to run.

The guide asks for interfaces to be generic, so other contracts can reuse them. That is why none of these designs is tied to a single project.

## Not covered yet

- Reference services for the oracle machine. Each design note lists what the service must do, but there is no code yet.
- Example contracts using each interface.
- Fee levels are proposals. The core team and computors set the real ones.

## Contributing

Issues and pull requests are welcome, especially answers to the open questions at the end of each design note.

## Licence

[MIT](LICENSE). Qubic, qubic/core and contract-verify belong to their respective authors. This project is independent and not affiliated with or endorsed by Qubic.
