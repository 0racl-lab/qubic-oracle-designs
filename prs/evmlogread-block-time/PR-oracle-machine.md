## What

`read_evm_log` returns two more fields at the end of its reply, matching the qubic/core EvmLogRead change ("EvmLogRead: add blockNumber and blockTimestamp to the reply"):

| Field | Bytes | Meaning |
|---|---|---|
| `blockNumber` | [440..448) | Block that includes the transaction |
| `blockTimestamp` | [448..456) | That block's timestamp, Unix seconds |

The reply grows from 440 to 456 bytes. Existing fields and result codes are unchanged.

## How the timestamp stays deterministic

- It is read from the block the receipt names, fetched with `eth_getBlockByHash` on every configured endpoint.
- The block's hash and number must match the agreed receipt.
- Every endpoint must report the same timestamp.

Anything else makes the service abstain, as with any other disagreement. The extra call is made only for answers that would succeed: finalized, successful receipts with a valid log index.

## Why

A contract accepting a token transfer as payment needs to know the transfer was made after its order was opened; a log alone can't show that. Details in the core PR.

## Tests

`test/selftest.py` (all pass):
- checks the two new fields;
- abstains when providers agree on the receipt but disagree on the block timestamp (new mock flag `--tamper-time`);
- abstains when the receipt's block can't be fetched;
- the existing checks updated for the 456-byte reply.

## Deployment

Deploy together with the core release carrying the new reply size; core rejects replies of the wrong size.

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_01QHXGjDwxBARUytE2smA8oq
