# UtxoTxRead: read a confirmed Dogecoin, Bitcoin or Litecoin payment

> **Status:** implemented as patches for both repositories, tested, not yet reviewed by the core or oracle team.
> - qubic/core: [`patches/qubic-core.patch`](../patches/qubic-core.patch), with the interface header [`oracle_interfaces/UtxoTxRead.h`](../oracle_interfaces/UtxoTxRead.h) at index 6
> - qubic/oracle-machine: [`patches/oracle-machine.patch`](../patches/oracle-machine.patch), service `oracles/read_utxo_tx`

**In one line:** a contract asks for one output of a Dogecoin (or Bitcoin, or Litecoin) transaction. Once the transaction is buried under enough blocks, the oracle returns that output's value and script, and the block it's in.

It is the UTXO sibling of EvmLogRead. EvmLogRead lets a Qubic contract prove that a USDC payment happened on an EVM chain; UtxoTxRead does the same for DOGE, BTC and LTC, with no bridge and no wrapped token.

## Why one interface for three chains

Dogecoin, Bitcoin and Litecoin share the same transaction format and the same node RPC (`getrawtransaction`, `getblockheader`). One service reads all three, and the query names the network.

Core's oracle guide asks for interfaces to be generic so other contracts can reuse them, so a separate "DogeTxRead" would just duplicate this. Dogecoin is the first network to target: it is the one Qubic already works with (Dogecoin mining via DogeShareValidation).

## What it unlocks

- **Peer-to-peer QU ⇄ DOGE (or BTC, LTC) trades.** The seller locks QU and the buyer pays DOGE to the seller's address. The contract releases the QU once the oracle confirms the payment. That is QPipe's model with Dogecoin in place of USDC.
- **Paying for Qubic services in DOGE:** a contract unlocks a service once a payment to a given address is confirmed.
- **DOGE or BTC-backed agreements:** escrow, deposits, or loans where the collateral payment happens on the other chain.

## How it works

1. A contract sends a query:
   - the network;
   - the transaction id;
   - which output (`vout`) to read;
   - how many confirmations it requires.
2. Every oracle machine asks its own nodes (`getrawtransaction txid 1`, then `getblockheader`).
3. If the transaction is unknown everywhere, it answers `TX_NOT_FOUND`. If it is in the mempool, reorganised away, or below the requested depth on any node, it answers `NOT_CONFIRMED`.
4. Otherwise it returns the output's value in base units, its raw `scriptPubKey`, the number of outputs, and the block's hash, height and timestamp. For Dogecoin, base units are koinu: 1 DOGE = 100,000,000.
5. On any failure every field except `code` is zero. If nodes disagree, or a node errors, the machine abstains and commits nothing.

## Query and reply

| Query field | Size | Meaning |
|---|---|---|
| `network` | 8 | 1 Bitcoin, **2 Dogecoin**, 3 Litecoin; 101/102/103 their test networks; 0 is invalid |
| `txid` | 32 | Transaction id, in the order explorers and the node show it |
| `outputIndex` | 8 | Which output to read |
| `minConfirmations` | 8 | From the network's minimum up to 1,000 |

| Network | Minimum depth | About |
|---|---|---|
| Bitcoin | 3 | 30 minutes |
| Dogecoin | 10 | 10 minutes |
| Litecoin | 6 | 15 minutes |
| Test networks | 1 | |

Query: 56 bytes. Reply: 208 bytes, made up of `code`, `blockHash`, `blockHeight`, `blockTime`, `outputCount`, `value` and up to 128 bytes of `script`.

**Result codes:** `SUCCESS`, `BAD_QUERY`, `NETWORK_UNSUPPORTED`, `TX_NOT_FOUND` (retriable), `NOT_CONFIRMED` (retriable), `OUTPUT_OUT_OF_RANGE`, `SCRIPT_TOO_LARGE`.

**Raw script, not an address.** The contract stores the exact `scriptPubKey` it expects and compares bytes. For a Dogecoin P2PKH address that is `76 a9 14 <20-byte hash> 88 ac`. The oracle stays free of address formats. An `OP_RETURN` output (up to 83 bytes) can be read the same way, so a payment can carry a trade reference.

## Why every machine gives the same answer

Once a block is buried under enough others, every honest node sees the same transaction in it. The depth rule is checked on **every** node a machine uses. A node a few blocks behind turns the answer into `NOT_CONFIRMED` rather than a different reply, and at the exact boundary machines may split, so the query times out and the contract retries.

Amounts are parsed as exact decimals and converted to base units only if exact. They never pass through floating point.

**Deliberately left out: whether the output has been spent since.** That changes over time and would break agreement.

## Fees

1,000 QU, the same as EvmLogRead.

## Rules for contracts using it

- **Tie each payment to one trade.** There is no EIP-3009-style signed nonce on these chains, so use one of:
  - a fresh receiving address per trade;
  - a unique amount, like QPipe's tag;
  - an `OP_RETURN` output carrying the trade id.

  Also record each `(txid, output)` accepted, so a payment can't be counted twice.
- **Choose the depth to match the value.** The minimums are floors, not recommendations for large amounts.
- **`TX_NOT_FOUND` and `NOT_CONFIRMED` are retriable,** never proof that a payment wasn't made.

## For oracle machine operators

Run your **own** nodes with `txindex=1`, at least two per network, and set `UTXO_RPC_URLS_DOGECOIN=http://user:pass@host:22555,...`. All of a machine's nodes must agree, or it abstains. The service is stdlib-only Python, framed exactly like `read_evm_log`.

## Tests

- **qubic/core:** 4 tests in `test/oracle_engine.cpp`, in the same style as EvmLogRead's:
  - registration at index 6;
  - pinned sizes and field offsets;
  - fee through the registry;
  - networks and minimum depths;
  - result codes and reply helpers;
  - deterministic query bytes.
- **oracle-machine:** a selftest against mock Dogecoin Core nodes, which use basic auth, HTTP-500 JSON-RPC errors and 8-decimal amounts. Its 33 checks cover:
  - exact koinu amounts, P2PKH and `OP_RETURN` scripts;
  - mempool, shallow and orphaned transactions;
  - a node 45 blocks behind;
  - disagreement, a dead node, wrong credentials, an inexact amount;
  - the TCP framing.
- `live_check.py` runs one real query against your own node.

## Open questions

- [ ] Are the minimum depths (Bitcoin 3, Dogecoin 10, Litecoin 6) right, or should they be higher?
- [ ] Should the service also accept an Electrum or Blockbook backend for operators who don't run full nodes?
- [ ] Is 1,000 QU the right fee for a UTXO read?
