## What

EvmLogRead's reply gains two fields at the end:

| Field | Bytes | Meaning |
|---|---|---|
| `blockNumber` | [440..448) | Block that includes the transaction |
| `blockTimestamp` | [448..456) | That block's timestamp, Unix seconds |

Every existing offset is unchanged; the reply grows from 440 to 456 bytes. `getBlockDateAndTime()` converts the timestamp to a QPI `DateAndTime`, so a contract can compare it directly with `qpi.now()` or a stored time.

## Why

A log shows that a transfer happened, not when. A contract that accepts a token transfer as payment for an order needs to know the transfer was made after the order was opened; otherwise an older, unrelated transfer between the same addresses can be claimed. Tokens with EIP-3009 can bind a payment to an order with a signed nonce, but tokens without it (USDT, for example) can't, so today they can't be used safely with EvmLogRead.

The block number also lets a contract order two payments, or check a block-based deadline.

## Tests

`test/oracle_engine.cpp`:
- `EvmLogReadOracle.DataContract` pins the new size (456) and the offsets of `data`, `blockNumber` and `blockTimestamp`.
- New `EvmLogReadOracle.BlockTimeAsDateAndTime`: known timestamps, a leap day, the epoch, "one second before an order opened", and no time from a failed reply.

The EvmLogRead tests were built and run on Linux against this branch (6/6 pass). Not yet run in the MSVC suite.

## Deployment

Core checks replies against the registered size, so this needs the matching oracle-machine change (qubic/oracle-machine: "read_evm_log: return blockNumber and blockTimestamp") deployed together. No contract in core uses EvmLogRead yet.

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_01QHXGjDwxBARUytE2smA8oq
