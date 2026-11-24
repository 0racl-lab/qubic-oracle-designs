# ContentRead: read a file by its fingerprint

> **Status:** implemented as patches for both repositories, tested, not yet reviewed by the core or oracle team.
> - qubic/core: [`patches/qubic-core/`](../patches/qubic-core), with the interface header [`oracle_interfaces/ContentRead.h`](../oracle_interfaces/ContentRead.h) at index 7
> - qubic/oracle-machine: [`patches/oracle-machine/`](../patches/oracle-machine), service `oracles/read_content`

**In one line:** a contract names a file by its SHA-256 hash; the oracle fetches it, checks the bytes match the hash, and returns the piece the contract asked for.

## What it unlocks

- **Data registries:** check that a contributed dataset really exists and is the size claimed before paying the contributor.
- **Manifests and configs:** a contract stores only a hash and reads one field (a licence, a version, a price list) when it needs it.
- **NFT and media checks:** confirm that metadata matches what was promised, and read its fields.
- **Storage markets:** pay storage providers in QU and spot-check that they still serve the files (see [Building on it](#building-on-it)).
- **Small tables and parameters:** pull lookup tables or model weights into a contract piece by piece.

## How it works

1. A contract sends a query: the file's SHA-256, a locator hint (IPFS CID, Arweave id, or none), a size limit, and what to return.
2. Every oracle machine fetches the file from the locator, through its operator's IPFS or Arweave gateways, or from its content mirrors by hash.
3. It never downloads more than `maxFileSize`. A source offering a bigger file counts as a miss.
4. It hashes the whole file with SHA-256. If the hash doesn't match, it discards the bytes and tries the next source.
5. Once it has matching bytes, it answers the requested mode:
   - **size:** just `fileSize`, proving the file is available right now;
   - **byteRange:** up to 512 bytes from `offset`;
   - **jsonField:** parses the file as strict I-JSON (RFC 7493) and returns one value, in RFC 8785 canonical form: sorted keys, no whitespace, ECMAScript number formatting, so `3.0` and `3` both come back as `3` on every machine.
6. On any failure it zeroes every field except `code`.
7. **Missing versus failing.** If every source answered definitively (404, wrong bytes, too large), the answer is `NOT_FOUND`. If any source timed out or errored, the machine abstains and commits nothing.

## Query and reply

| Query field | Size | Meaning |
|---|---|---|
| `sha256` | 32 | SHA-256 of the complete file. **The truth.** |
| `locatorKind` | 8 | 0 = known mirrors only, 1 = IPFS CID, 2 = Arweave id |
| `locatorLen`, `locator` | 8 + 64 | Where to look, as ASCII. **Only a hint.** |
| `maxFileSize` | 8 | Refuse anything bigger (max 64 MiB). Also sets the fee. |
| `mode` | 8 | 1 = size, 2 = byte range, 3 = JSON field |
| `offset`, `length` | 8 + 8 | Byte range (length 1–512) |
| `jsonPath` | 64 | Dot path such as `meta.licence` or `items.3.name`; empty means the whole document |

Fields a mode doesn't use must be zero, so two equal requests are always equal bytes.

Query: 208 bytes. Reply: `code`, `fileSize`, `dataLen` and 512 bytes of `data`, 536 bytes in all. Both are well inside core's 1,008-byte limit.

**Result codes:** `SUCCESS`, `BAD_QUERY`, `NOT_FOUND` (retriable), `BAD_RANGE`, `NOT_JSON`, `NO_FIELD`, `VALUE_TOO_LARGE`.

**No "too large" code.** An earlier sketch had one. But a file over `maxFileSize` is never fully downloaded, so it can never be checked against the hash, and the oracle would be asserting something about bytes it hasn't verified. An oversized file is simply `NOT_FOUND`.

**Why the hash and the CID are separate:** for anything above a small size, an IPFS CID fingerprints IPFS's internal chunk layout, not the file itself. Using SHA-256 of the full bytes as the truth, and the CID only as a place to look, keeps checking simple and works the same for IPFS, Arweave or any mirror.

## Why every machine gives the same answer

The hash fixes the bytes. Any honest machine that finds the file holds identical bytes, so it computes the same size, range or JSON value. Every code except `NOT_FOUND` is a property of those bytes. The only thing that can differ between machines is whether the file was found in time.

When machines split on that, the query simply times out without a quorum. That is the same behaviour as EvmLogRead near the finality boundary.

## Fees

50 QU plus 10 QU per started MiB of `maxFileSize`: 60 QU for a file up to 1 MiB, 690 QU at the 64 MiB maximum. Tying the fee to the limit stops one cheap query from making every oracle machine download a large file.

## Rules for contracts using it

- **`NOT_FOUND` means "not available right now", never "does not exist".** Retry later. Never punish anyone, forfeit a deposit or close a deal on a single `NOT_FOUND`.
- **Content is public.** Anyone who knows the hash can read the file through the oracle. Never use it for private or personal data.
- **Keep `maxFileSize` as small as the use allows.** It keeps fees low.
- **Check `fileSize`** as well as the data, if the size matters.

## Security and abuse

- **Fetch limits:** the size cap, a time limit per fetch, and GET only. Machines never fetch a URL the query supplies, only content by identifier through their own gateways.
- **No poisoning:** a malicious gateway can only waste time. Wrong bytes fail the hash check and are thrown away.
- **Legal content:** an oracle machine fetches whatever matches a hash. Operators may want a denylist of hashes, applied the same way on every machine so replies stay identical.

## Limits

- **Availability is the weak point.** If nobody pins the file, the oracle can't answer. Important files need pinning in several places.
- **512 bytes per reply.** Larger reads take several queries (byte ranges) or a Merkle design (below).
- **JSON mode adds complexity** to the oracle machine. A first version could ship with size and byte range only.

## Building on it

- **Version 2, items by Merkle root.** The contract stores one root for a dataset of millions of items, and the oracle returns item *i* with its proof checked against the root. Large datasets work without storing anything big on-chain.
- **Data registry contract:** records datasets by fingerprint, contributors and licences, and pays royalties when data is used.
- **Storage market contract:** users prepay QU to keep a file available for N epochs. Providers post a deposit and get paid each epoch, and the contract spot-checks random pieces through ContentRead. This proves a provider could serve the pieces when asked, not that it stores the whole file. Full proof-of-storage would need [ProofVerify](proof-verify.md).

## Tests

- **qubic/core:** 4 tests in `test/oracle_engine.cpp`:
  - registration at index 7;
  - pinned sizes and all field offsets;
  - the fee at each step (1 byte, 1 MiB, 1 MiB + 1, 64 MiB) and through the registry;
  - result codes and reply helpers;
  - deterministic query bytes.
- **oracle-machine:** a selftest against mock gateways, one honest, one that lies and one that fails. Its 45 checks cover:
  - every mode and every source type;
  - a lying gateway skipped in favour of an honest copy;
  - wrong bytes never returned;
  - the size limit both ways;
  - every result code and 14 kinds of malformed query, including path characters in a CID;
  - abstaining when a failing gateway could be hiding the file;
  - RFC 8785's own example and number vectors;
  - the TCP framing.
- `live_check.py` reads a real file through public IPFS gateways.

## Open questions

- [ ] Which gateways and mirrors should oracle machines use, and would a Qubic-run mirror help availability?
- [ ] How long may a fetch take inside one oracle round?
- [ ] Is JSON mode worth it in version 1, or are size and byte ranges enough?
- [ ] Should SHA-256 be the only hash, or should K12 (Qubic's native hash) be offered too?
