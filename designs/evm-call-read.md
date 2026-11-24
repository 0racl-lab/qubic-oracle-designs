# EvmCallRead: read EVM contract state at a finalized block

> **Status:** design sketch. Not part of qubic/core, not reviewed by the core or oracle team.
> Interface header: [`oracle_interfaces/EvmCallRead.h`](../oracle_interfaces/EvmCallRead.h)

**In one line:** a contract asks for a read-only call to an EVM contract at a specific finalized block (for example `balanceOf(x)` on Base at block N), and the oracle returns the raw result.

EvmLogRead proves that something **happened** (an event was emitted). EvmCallRead reads what something **is** at a fixed moment: a balance, an owner, a total supply, a price.

## What it unlocks

- **Token-gating:** let only holders of a token or NFT on another chain use a Qubic service.
- **Ownership checks:** confirm who owns an NFT before paying a royalty or granting a licence.
- **Collateral and reserves:** check a vault's balance or a pool's reserves on an EVM chain before acting.
- **Cross-chain governance:** count voting power held on another chain at a snapshot block.
- **Current ENS records:** read what a name points to now, rather than what it was once set to.

## How it works

1. A contract sends a query: the chain id, a block number, the target contract, and ABI-encoded calldata (selector plus arguments, up to 512 bytes).
2. Every oracle machine checks that the block is at or below the chain's **finalized** block. If not, it answers `BLOCK_NOT_FINALIZED`.
3. It runs `eth_call` against that exact block with a fixed gas cap (10 million) and no state overrides.
4. It returns the block's hash, whether the call reverted, and up to 512 bytes of return data (or revert data).
5. On any failure it zeroes every field except `code`.

## Query and reply

| Query field | Size | Meaning |
|---|---|---|
| `chainId` | 8 | EVM chain id, the same numbers as EvmLogRead (1, 10, 137, 8453, 42161, 43114, …) |
| `blockNumber` | 8 | Block whose state to read; must be finalized |
| `to` | 32 | Contract to call, as a right-aligned 32-byte word |
| `calldataLen`, `calldata` | 8 + 512 | ABI-encoded call |

Query: 568 bytes. Reply: 568 bytes, made up of `code`, `blockHash`, `reverted`, `dataLen` and 512 bytes of `data`.

**Result codes:** `SUCCESS`, `BAD_QUERY`, `CHAIN_UNSUPPORTED`, `BLOCK_NOT_FINALIZED` (retriable), `BLOCK_TOO_OLD`, `OUT_OF_GAS`, `RETURN_TOO_LARGE`.

A call that **reverts** is a deterministic result, not a failure. It comes back as `SUCCESS` with `reverted = 1` and the revert data, so a contract can tell "the call failed" apart from "the oracle couldn't answer".

## Why every machine gives the same answer

State at a fixed, finalized block never changes. With the same block, the same calldata, the same gas cap and no overrides, every honest node computes the same result. The returned `blockHash` ties the answer to one chain history.

**The state window.** Machines aren't expected to run full archive nodes. They must answer for blocks from roughly the last day (`STATE_WINDOW_SECONDS`) and may answer `BLOCK_TOO_OLD` beyond that. Near the edge of the window machines may split, and the query times out. Contracts should ask for recent finalized blocks.

## Fees

1,000 QU, the same as EvmLogRead.

## Rules for contracts using it

- **Pick a recent finalized block,** and remember the answer is "as of that block". A balance can change one block later.
- **Use the right block number for the chain.** On rollups like Base and Arbitrum, finality comes from Ethereum and takes 10–30 minutes. Polygon and Avalanche finalize in seconds.
- **Decode defensively:** check `reverted` and `dataLen` before reading the result.
- **Only call contracts you trust to behave.** A malicious contract can return anything it likes; the oracle only proves what the call returned.

## Security and abuse

- **Gas cap** limits how much work one query can cause on each machine.
- **Read-only:** `eth_call` can't change any chain's state.
- **Node diversity:** as with EvmLogRead, machines should use their own nodes or several independent providers.

## Limits

- **512 bytes of return data.** Enough for balances, owners and small structs, but not for large arrays.
- **Recent blocks only** without archive nodes.
- **Freshness:** "now" in practice means the latest finalized block, minutes behind the chain tip on rollups.

## Open questions

- [ ] Is about a day the right state window, or should it be measured in blocks per chain?
- [ ] Should the gas cap be lower to protect machines, or configurable within a range?
- [ ] Should batching several calls into one query (multicall) be supported in version 1?
