# ProofVerify: check zero-knowledge proofs for Qubic contracts

> **Status:** design sketch. Not part of qubic/core, not reviewed by the core or oracle team.
> Interface header: [`oracle_interfaces/ProofVerify.h`](../oracle_interfaces/ProofVerify.h)

**In one line:** oracle machines verify a zero-knowledge proof off-chain, and computors agree on one bit: valid or not.

Qubic contracts can't run pairing-based proof verifiers themselves. They don't need to. Verification is pure maths on fixed bytes, so every honest oracle machine reaches the same verdict. That one interface would bring zero-knowledge proofs to Qubic without changing the contract language.

## What it unlocks

- **Fiat payments proven from the bank's own website (zkTLS).** A buyer proves "my bank showed a payment of £50 to account X with reference R" without revealing their login. That is the missing piece for peer-to-peer fiat ⇄ QU trades.
- **Web2 account proofs:** prove you own a GitHub, X or email account, or that an account meets a condition, without sharing credentials.
- **Privacy-preserving checks:** prove age, residency or membership without revealing the underlying data.
- **Storage proofs:** prove a provider really stores a whole file, not just pieces of it on request (see [ContentRead](content-read.md)).
- **Validity proofs from other systems:** check rollup or bridge proofs produced elsewhere.

## How it works

1. A contract sends a query:
   - the proof system, for example Groth16 over BN254;
   - the SHA-256 of the verifying key, plus where to fetch it;
   - up to 8 public inputs;
   - the proof itself, inline (up to 256 bytes) or by SHA-256 plus a locator;
   - a 64-byte `context` tag.
2. Every oracle machine fetches the verifying key (and the proof, if given by hash), checking each against its SHA-256, the same way ContentRead does.
3. It runs the standard verifier for that system on the key, the public inputs and the proof.
4. It replies `CHECKED` with `valid = 1` or `0`, echoing the key hash and the context tag.
5. If the key or proof can't be found or doesn't parse, it returns a failure code and zeroes the rest.

## Query and reply

| Query field | Size | Meaning |
|---|---|---|
| `system` | 8 | 1 = Groth16 / BN254, 2 = PLONK (KZG) / BN254 |
| `vkSha256` | 32 | Which verifying key, i.e. which statement is being proven |
| `vkLocatorKind`, `vkLocator` | 8 + 64 | Where to fetch the key (a hint; the hash decides) |
| `publicInputCount`, `publicInputs` | 8 + 256 | Up to 8 field elements, 32 bytes each |
| `proofLen`, `proof` | 8 + 256 | Inline proof; a Groth16 proof is 256 bytes uncompressed |
| `proofSha256`, `proofLocatorKind`, `proofLocator` | 32 + 8 + 64 | Larger proofs, by hash |
| `context` | 64 | Contract-chosen tag echoed in the reply |

Query: 808 bytes. Reply: 112 bytes, made up of `code`, `valid`, `vkSha256` and `context`.

**Result codes:** `CHECKED` (see `valid`), `BAD_QUERY`, `SYSTEM_UNSUPPORTED`, `NOT_FOUND` (retriable), `MALFORMED`. A proof that parses but doesn't verify is **not** a failure code: it comes back as `CHECKED` with `valid = 0`.

## Why every machine gives the same answer

Verifying a proof is deterministic: the same key, inputs and proof always give the same verdict. Keys and large proofs are fetched by hash, so every machine checks the same bytes. As with ContentRead, the only thing that can differ is whether a fetch succeeded in time.

## What the contract must decide

The oracle only says "this proof is valid for this key and these inputs". The contract gives that meaning:

- **Which keys it trusts.** A verifying key defines the statement being proven. A contract should accept only key hashes fixed in its code or approved by shareholder vote. Otherwise anyone could prove a meaningless statement.
- **What the public inputs must be.** For a zkTLS bank proof, they would include the bank's identity, the amount, the receiving account and a trade reference. The contract checks they match the trade.
- **Freshness and uniqueness.** Put the trade id in a public input and in `context`, and record used proofs, so one proof can't settle two trades.

## Fees

500 QU with the proof inline, 750 QU when it has to be fetched by hash. Pairing checks cost more than plain reads, and fetching adds work.

## Security and abuse

- **Trusted setup.** Groth16 needs a per-circuit setup ceremony. A compromised setup could allow false proofs, so contracts should only trust keys from well-run ceremonies, or prefer PLONK's universal setup.
- **Verifier bugs.** All machines running the same verifier library share its bugs. Using well-audited libraries, and ideally more than one implementation, reduces the risk.
- **zkTLS trust.** zkTLS proofs rest on assumptions about the TLS session and, in some designs, a notary. Those assumptions must be stated per key.
- **Fetch limits:** key and proof downloads are capped (1 MiB) and time-limited.

## Limits

- **8 public inputs,** enough for most statements; bigger ones can hash their inputs into one.
- **Two proof systems at first.** STARKs and specific zkTLS presentation formats would be added as new `system` values, each needing a verifier in the oracle machine.
- **Off-chain verification.** The guarantee is "the oracle quorum verified it", not "the contract verified it". That is the same trust model as every other Qubic oracle.

## Open questions

- [ ] Which verifier libraries would oracle machine operators accept?
- [ ] Which zkTLS system to support first (TLSNotary, Reclaim, zkPass, …), and in what proof format?
- [ ] Should there be a shared on-chain registry of approved verifying keys, so contracts don't each maintain their own?
- [ ] Is 8 public inputs the right limit?
