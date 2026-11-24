# EvmCallRead: read EVM contract state at a finalized block

> **Status:** implemented as patches for both repositories, tested, not yet reviewed by the core or oracle team.
> - qubic/core: [`patches/qubic-core/`](../patches/qubic-core), with the interface header [`oracle_interfaces/EvmCallRead.h`](../oracle_interfaces/EvmCallRead.h) at index 5
> - qubic/oracle-machine: [`patches/oracle-machine/`](../patches/oracle-machine), service `oracles/read_evm_call`

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
2. Every oracle machine checks that the block is at or below the **finalized** head on every one of its RPC endpoints. If not, it answers `BLOCK_NOT_FINALIZED`.
3. It resolves the block number to a block hash on every endpoint and requires them to agree.
4. It runs `eth_call` pinned to that hash (EIP-1898, `requireCanonical`), from the zero address, with exactly 10 million gas and no state overrides. Every endpoint must return the same outcome, or the machine abstains.
5. It returns the block's hash, whether the call reverted, and up to 512 bytes of return data (or revert data).
6. On any failure it zeroes every field except `code`.

## Query and reply

| Query field | Size | Meaning |
|---|---|---|
| `chainId` | 8 | EVM chain id, the same numbers as EvmLogRead (1, 10, 137, 8453, 42161, 43114, …) |
| `blockNumber` | 8 | Block whose state to read; must be finalized |
| `to` | 32 | Contract to call, as a right-aligned 32-byte word |
| `calldataLen`, `calldata` | 8 + 512 | ABI-encoded call |

Query: 568 bytes. Reply: 568 bytes, made up of `code`, `blockHash`, `reverted`, `dataLen` and 512 bytes of `data`.

**Result codes:** `SUCCESS`, `BAD_QUERY`, `CHAIN_UNSUPPORTED`, `BLOCK_NOT_FINALIZED` (retriable), `BLOCK_TOO_OLD`, `OUT_OF_GAS`, `RETURN_TOO_LARGE`.

A call that **reverts** is a deterministic result, not a failure. It comes back as `SUCCESS` with `reverted = 1` and the revert data, so a contract can tell "the call failed" apart from "the oracle couldn't answer". Other EVM execution failures (invalid opcode, bad jump, stack errors) come back the same way with empty data. Any node error the service doesn't recognise makes it abstain rather than guess.

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

## Tests

- **qubic/core:** 4 tests in `test/oracle_engine.cpp`, in the same style as EvmLogRead's:
  - registration at index 5;
  - pinned sizes and field offsets;
  - fee through the registry;
  - the same chain ids as EvmLogRead;
  - result codes and reply helpers;
  - deterministic `balanceOf` query bytes.
- **oracle-machine:** a selftest against mock EVM nodes that only answer the exact required call (zero sender, gas cap, hash pin). Its 29 checks cover:
  - `balanceOf`, a revert with `Error(string)` data, an invalid opcode, an address with no code;
  - not finalized, too old, out of gas, oversized return;
  - five malformed queries;
  - disagreement, an unrecognised node error, a dead endpoint;
  - `latest - N` finality and the TCP framing.
- `live_check.py` runs one real `balanceOf` against live endpoints.

## Open questions

- [ ] Is about a day the right state window, or should it be measured in blocks per chain?
- [ ] Should the gas cap be lower to protect machines, or configurable within a range?
- [ ] Should batching several calls into one query (multicall) be supported in version 1?
