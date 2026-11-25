# Pull request: block number and block timestamp in EvmLogRead

Two small pull requests that must land together:

| Repository | Target branch | Patch | Based on |
|---|---|---|---|
| qubic/core | `develop` | [`qubic-core.patch`](qubic-core.patch) | `5dd7363` |
| qubic/oracle-machine | `main` | [`oracle-machine.patch`](oracle-machine.patch) | `72192da` |

`check/evmlogread_pr.sh` applies both to fresh clones, builds core's EvmLogRead tests on Linux and runs the service selftest.

## What changes

EvmLogRead's reply gains two fields **at the end**, so every existing offset stays where it is:

| Field | Bytes | Meaning |
|---|---|---|
| `blockNumber` | [440..448) | Block that includes the transaction |
| `blockTimestamp` | [448..456) | That block's timestamp, Unix seconds |

The reply grows from 440 to 456 bytes. A helper, `getBlockDateAndTime()`, converts the timestamp to a QPI `DateAndTime` for comparison with `qpi.now()`.

The service reads the timestamp from the block the receipt names, fetched by hash on every endpoint. The block's hash and number must match the receipt, and every endpoint must report the same timestamp, or it abstains.

## Why

A log shows that a transfer happened, not when. A contract accepting a token transfer as payment for an order needs to know the transfer was made after the order was opened. Without that, an attacker can point at an older, unrelated transfer between the same addresses and claim it. Tokens with EIP-3009 can work around this with a signed nonce, but tokens without it, such as USDT, can't.

## Deployment note

Core checks each reply against the registered size, so the core release and the oracle-machine update have to go out together: an old service's 440-byte reply wouldn't be accepted by a new core, and the other way round. No contract in core uses EvmLogRead today (checked on `develop` at `5dd7363`), so nothing else needs changing.

## Relation to EvmTxRead

[EvmTxRead](../../designs/evm-tx-read.md) answers the same "when" question as a separate interface and leaves EvmLogRead untouched. The two are alternatives for the payment case; EvmTxRead also covers native-coin payments, sender checks and reverted transactions. If the oracle team prefers not to change EvmLogRead's layout, EvmTxRead is the fallback.

## Opening the pull requests

From a fork of each repository (GitHub's "Fork" button), with the GitHub CLI:

```sh
# qubic/core -> develop
git clone https://github.com/<your-user>/core.git && cd core
git remote add upstream https://github.com/qubic/core.git && git fetch upstream develop
git checkout -b evmlogread-block-time upstream/develop
git am /path/to/qubic-oracle-designs/prs/evmlogread-block-time/qubic-core.patch
git push -u origin evmlogread-block-time
gh pr create --repo qubic/core --base develop --title "EvmLogRead: add blockNumber and blockTimestamp to the reply" --body-file /path/to/qubic-oracle-designs/prs/evmlogread-block-time/PR-core.md

# qubic/oracle-machine -> main
git clone https://github.com/<your-user>/oracle-machine.git && cd oracle-machine
git remote add upstream https://github.com/qubic/oracle-machine.git && git fetch upstream main
git checkout -b evmlogread-block-time upstream/main
git am /path/to/qubic-oracle-designs/prs/evmlogread-block-time/oracle-machine.patch
git push -u origin evmlogread-block-time
gh pr create --repo qubic/oracle-machine --base main --title "read_evm_log: return blockNumber and blockTimestamp" --body-file /path/to/qubic-oracle-designs/prs/evmlogread-block-time/PR-oracle-machine.md
```

After both are open, add a link to each from the other's description.
