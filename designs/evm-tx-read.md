# EvmTxRead: when and by whom an EVM transaction happened

> **Status:** implemented as patches for both repositories, tested, not yet reviewed by the core or oracle team.
> - qubic/core: [`patches/qubic-core/`](../patches/qubic-core), with the interface header [`oracle_interfaces/EvmTxRead.h`](../oracle_interfaces/EvmTxRead.h) at index 8
> - qubic/oracle-machine: [`patches/oracle-machine/`](../patches/oracle-machine), service `oracles/read_evm_tx`

**In one line:** a contract gives a chain id and a transaction hash, and the oracle returns where and when that finalized transaction was included, who sent it, to whom, the native value, and whether it succeeded.

It is the sibling of EvmLogRead. EvmLogRead returns **what** a transaction emitted: one raw log, such as an ERC-20 `Transfer`. EvmTxRead returns **when** and **by whom**. EvmLogRead's reply deliberately holds only the log, so on its own a contract can't tell when a transfer happened.

## What it unlocks

- **Payments in any ERC-20 token, including ones without signed transfers.** Accept a plain `Transfer` only if it was made after an order was opened. Without the time, an attacker can point at an old, unrelated transfer to the same recipient and claim it as payment. Tokens with EIP-3009 (`transferWithAuthorization`), like USDC, can bind a payment to an order with a signed nonce instead. Tokens without it, like USDT, need the time.
- **Deadlines:** pay out only if something happened on another chain before a cut-off.
- **Ordering:** settle disputes over which of two transactions came first, using block number and position.
- **Sender checks:** confirm which account sent a transaction, for example that a deployment or a native-coin payment came from a known address.
- **Native coin payments:** ETH, BNB, POL or AVAX sent in a plain transfer has no log at all. EvmTxRead returns its `value`, `from`, `to` and `status` directly.

## How it works

1. A contract sends a query: the chain id and the transaction hash.
2. Every oracle machine asks each of its RPC endpoints for the transaction, its receipt, and the block the receipt names (by hash). For each endpoint these must tell one consistent story: the same block hash, block number, position and sender. If they don't, the machine abstains.
3. The block must be at or below the **finalized** head on every endpoint. If not, the answer is `TX_NOT_FINALIZED`.
4. Every endpoint must return identical results, including "unknown" and "pending". Otherwise the machine abstains.
5. It returns the fields below. On any failure it zeroes every field except `code`.

## Query and reply

| Query field | Size | Meaning |
|---|---|---|
| `chainId` | 8 | EVM chain id, the same numbers as EvmLogRead (1, 10, 56, 137, 250, 8453, 42161, 43114, Sepolia) |
| `txHash` | 32 | Transaction hash |

Query: 40 bytes.

| Reply field | Size | Meaning |
|---|---|---|
| `code` | 8 | Result code |
| `blockNumber` | 8 | Block that includes the transaction |
| `blockTimestamp` | 8 | That block's timestamp, seconds since 1970 (UTC) |
| `transactionIndex` | 8 | Position in the block |
| `blockHash` | 32 | Hash of that block |
| `from` | 32 | Sender, right-aligned |
| `to` | 32 | Recipient, right-aligned; all zero for a contract creation |
| `value` | 32 | Native value in wei (uint256, big-endian) |
| `nonce` | 8 | Sender's nonce |
| `status` | 8 | 1 success, 0 reverted |
| `txType` | 8 | 0 legacy, 1 access list, 2 EIP-1559, 3 blob, 4 EIP-7702 |
| `logCount` | 8 | Logs in the receipt: valid `logIndex` values for EvmLogRead |

Reply: 192 bytes.

**Result codes:** `SUCCESS`, `BAD_QUERY` (zero hash), `CHAIN_UNSUPPORTED`, `TX_NOT_FOUND` (retriable), `TX_NOT_FINALIZED` (retriable).

**A reverted transaction is an answer.** It comes back as `SUCCESS` with `status = 0`, so a contract can tell "this payment failed" apart from "not there yet". EvmLogRead folds reverts into "not finalized"; EvmTxRead keeps them apart.

**`getBlockDateAndTime(reply, out)`** converts `blockTimestamp` to a QPI `DateAndTime`, so a contract can compare it directly with `qpi.now()` or a time it stored when an order was opened.

## Using it with EvmLogRead: a token payment made after an order

1. When the order is opened, the contract stores `qpi.now()`, the payer's address, the recipient and the amount.
2. The payer sends the tokens and submits the transaction hash and log index.
3. The contract queries **EvmLogRead** for the log: it must be a `Transfer` from the token contract, with `from`, `to` and the amount it expects.
4. The contract queries **EvmTxRead** for the same hash: `status` must be 1, and the block time must be after the order was opened.
5. The contract records the transaction hash as used, so it can never pay out twice.

Use the `from` in the `Transfer` log, not the transaction's `from`, to decide who paid. They differ when a relayer or smart-contract wallet sends the transaction for the payer.

The two queries cost 2,000 QU together, and they can be sent in the same tick.

## Why every machine gives the same answer

A transaction in a finalized block never moves, and its block's timestamp never changes. Each machine checks that its transaction, receipt and block agree with each other, and that every endpoint agrees. A machine whose node has not seen the block yet sees "unknown" where others see the transaction, so it abstains rather than answering differently.

## Fees

1,000 QU, the same as EvmLogRead and EvmCallRead.

## Rules for contracts using it

- **Check `status`.** `SUCCESS` means "the transaction is final", not "the transaction succeeded".
- **Block time is the chain's clock, not the oracle's.** EVM block timestamps are set by block producers within small limits. Compare with a margin of a minute or so rather than to the second.
- **On rollups, finality takes longer.** Base, Arbitrum and Optimism finalize through Ethereum, so expect 10–30 minutes before an answer.
- **Retry codes 3 and 4 later.** They describe the moment, not the transaction.
- **Store used transaction hashes** so one transaction can't be claimed twice.

## Why a new interface rather than extending EvmLogRead

- EvmLogRead stays exactly as it is. Its README describes it as deliberately limited to the raw log, and existing contracts keep working with no version change.
- It fits the existing pattern of one small interface per job: a 40-byte query and a 192-byte reply.
- It's useful on its own, for native coin payments, deadlines and sender checks, with no log involved.

## Limits

- **No input data.** The calldata isn't returned; use EvmLogRead for what a transaction did.
- **No gas or fee fields.** Gas used and gas price aren't needed to decide whether or when a payment happened, so they're left out.
- **Finalized only,** so minutes behind the chain tip.

## Tests

- **qubic/core:** 5 tests in `test/oracle_engine.cpp`, in the style of EvmLogRead's:
  - registration at index 8, pinned sizes and every field offset;
  - fee through the registry, and the same chain ids as EvmLogRead;
  - result codes and reply helpers (a reverted transaction is final but not valid);
  - block time to `DateAndTime`, including a leap day and the comparison "one second before an order opened";
  - deterministic query bytes.
- **oracle-machine:** a selftest against mock EVM nodes. Its 33 checks cover:
  - a finalized token transfer, field by field;
  - a reverted transaction, a contract creation with a 5 ETH value, a legacy transaction without a type field;
  - unknown, pending and not-yet-finalized transactions, an unconfigured chain, a zero hash, a wrong payload size;
  - a lagging node that has the transaction, and `latest - N` finality on both sides of the boundary;
  - abstaining on a provider whose timestamp differs, a node that hasn't seen the transaction, a missing receipt, a receipt naming another block, a block that can't be fetched, and a dead endpoint;
  - the TCP framing.
- `live_check.py` runs one real query against live endpoints.

## Open questions

- [ ] Should EvmLogRead and EvmTxRead be answerable in one query (log plus transaction) to halve the fee for the common payment case?
- [ ] Should the reply include the transaction's input selector (first 4 bytes), so a contract can see which function was called without EvmLogRead?
- [ ] Is `latest - N` finality acceptable for chains without a `finalized` tag, as with EvmLogRead?
