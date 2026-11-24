# BtcTxRead: read a confirmed Bitcoin payment

> **Status:** design sketch. Not part of qubic/core, not reviewed by the core or oracle team.
> Interface header: [`oracle_interfaces/BtcTxRead.h`](../oracle_interfaces/BtcTxRead.h)

**In one line:** a contract asks for one output of a Bitcoin transaction. Once the transaction is buried under enough blocks, the oracle returns that output's value and script, and the block it's in.

It is the Bitcoin sibling of EvmLogRead. EvmLogRead lets a Qubic contract prove that a USDC payment happened on an EVM chain; BtcTxRead does the same for BTC, with no bridge and no wrapped token.

## What it unlocks

- **Peer-to-peer QU ⇄ BTC trades.** The seller locks QU and the buyer pays BTC to the seller's address. The contract releases the QU once the oracle confirms the payment.
- **BTC-backed agreements:** escrow, deposits, or loans where the collateral payment happens on Bitcoin.
- **Paying for Qubic services in BTC:** a contract unlocks a service once a BTC payment to a given address is confirmed.

## How it works

1. A contract sends a query: the network, the transaction id, which output (`vout`) to read, and how many confirmations it requires (3 to 100; 6 is customary).
2. Every oracle machine looks the transaction up on its own Bitcoin node.
3. If the transaction isn't in a block yet, or its block has fewer confirmations than asked for, it answers `NOT_CONFIRMED`.
4. Otherwise it returns the output's value in satoshis, its raw `scriptPubKey`, the number of outputs, and the block's hash, height and timestamp.
5. On any failure it zeroes every field except `code`.

## Query and reply

| Query field | Size | Meaning |
|---|---|---|
| `network` | 8 | 0 = mainnet, 1 = testnet4, 2 = signet |
| `txid` | 32 | Transaction id, in the order block explorers display it |
| `outputIndex` | 8 | Which output to read |
| `minConfirmations` | 8 | Depth required before answering (3–100) |

Query: 56 bytes. Reply: 208 bytes, made up of `code`, `blockHash`, `blockHeight`, `blockTime`, `outputCount`, `value` and up to 128 bytes of `script`.

**Result codes:** `SUCCESS`, `BAD_QUERY`, `NETWORK_UNSUPPORTED`, `TX_NOT_FOUND` (retriable), `NOT_CONFIRMED` (retriable), `OUTPUT_OUT_OF_RANGE`, `SCRIPT_TOO_LARGE`.

**Raw script, not an address.** Bitcoin has several address formats (legacy, SegWit, Taproot). The contract stores the exact `scriptPubKey` it expects, for example the seller's, and compares bytes. The oracle stays free of address parsing. An `OP_RETURN` output (up to 83 bytes) can be read the same way, so a payment can carry a trade reference.

## Why every machine gives the same answer

Once a block is buried under several more, it is effectively final, so every honest node sees the same transaction in the same block. The confirmation threshold plays the role EvmLogRead's "finalized block" rule plays on EVM chains. Near the threshold, machines may disagree for a moment; the query then times out and the contract retries.

**Deliberately left out: whether the output has been spent since.** That changes over time and would break agreement. A contract that needs "unspent now" must be designed differently.

## Fees

1,000 QU, the same as EvmLogRead. Each query means every oracle machine consults a full Bitcoin node.

## Rules for contracts using it

- **Tie each payment to one trade.** On EVM, QPipe gets this from EIP-3009's signed nonce. Bitcoin has nothing equivalent, so a contract needs another way to make a payment unique. Options:
  - a fresh receiving script per trade, for example a different address from the seller's wallet;
  - a unique amount, as QPipe's tag does;
  - an `OP_RETURN` output carrying the trade id.

  It must also record each `(txid, output)` it has accepted, so the same payment can't be counted twice.
- **Choose the depth to match the value.** Six confirmations take about an hour. For large amounts, ask for more.
- **`TX_NOT_FOUND` and `NOT_CONFIRMED` are retriable,** never proof that a payment wasn't made.

## Security and abuse

- **Deep reorganisations** beyond the requested depth would break the finality assumption. That is the same trust assumption every Bitcoin exchange makes.
- **Node diversity:** oracle machines should run their own full nodes, not shared third-party APIs, so one provider can't decide the answer.

## Limits

- **Slow:** an hour or more for 6 confirmations, against seconds for Polygon or Avalanche through EvmLogRead.
- **One output per query.** Checking several outputs takes several queries.
- **No mempool or spent-state information,** by design.

## Open questions

- [ ] Which Bitcoin node software and versions should oracle machines run?
- [ ] Is 3 the right minimum depth, or should the minimum be higher?
- [ ] Should Litecoin or Dogecoin, which share Bitcoin's transaction format, reuse this interface with a `network` value?
