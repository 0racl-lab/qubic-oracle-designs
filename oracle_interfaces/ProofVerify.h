using namespace QPI;

/**
* Oracle interface "ProofVerify" (see Price.h for general documentation about oracle interfaces).
*
* DESIGN SKETCH - not part of qubic/core. See designs/proof-verify.md.
*
* Verify a zero-knowledge proof OFF-chain and return valid / invalid. Qubic contracts can't run
* pairing-based verifiers themselves, but oracle machines can, and computors then only need to
* agree on one bit. The query names the proof system, the verifying key (by SHA-256, fetched like
* ContentRead), the public inputs, and the proof (inline if small, or by SHA-256 if large).
*
* What the proof is ABOUT is defined by the verifying key's circuit: for example "this TLS session
* with bank.example showed a payment of X to account Y" (zkTLS), "this person is over 18" or "this
* Merkle path is in that tree". The contract decides which verifying keys it trusts (normally a list
* fixed in code or set by shareholder vote) and what the public inputs must be.
*
* Determinism (see Price.h): verification is pure mathematics on fixed bytes. The only thing that
* can differ between machines is fetching a key or proof by hash, reported as RESULT_NOT_FOUND.
* On failure all reply fields except code MUST be zero.
*/
struct ProofVerify
{
    //-------------------------------------------------------------------------
    // Mandatory oracle interface definitions

    /// Oracle interface index
    static constexpr uint32 oracleInterfaceIndex = ORACLE_INTERFACE_INDEX;

    //--- Proof systems (each is one verifier implementation in the oracle machine)
    struct System
    {
        static constexpr uint64 groth16Bn254 = 1;   ///< Groth16 over BN254 (snarkjs / gnark / circom); inline proof 256 bytes uncompressed
        static constexpr uint64 plonkBn254   = 2;   ///< PLONK with KZG over BN254 (snarkjs / gnark format)
        // add new proof systems above this line (e.g. a STARK or a specific zkTLS presentation format)
    };

    //--- Where large items live (same meaning as ContentRead::LocatorKind)
    struct LocatorKind
    {
        static constexpr uint64 none    = 0;
        static constexpr uint64 ipfsCid = 1;
        static constexpr uint64 arweave = 2;
    };

    //--- Result codes returned in OracleReply.code. 0 means the proof was checked; see valid.
    //    Any non-zero value is a failure reason and implies all other reply fields are all-zero.
    static constexpr uint64 RESULT_CHECKED            = 0;  ///< verification ran; valid says whether it passed
    static constexpr uint64 RESULT_BAD_QUERY          = 1;  ///< malformed query (zero vk hash, too many inputs, ...)
    static constexpr uint64 RESULT_SYSTEM_UNSUPPORTED = 2;  ///< proof system not served
    static constexpr uint64 RESULT_NOT_FOUND          = 3;  ///< key or proof bytes matching the hash not found in time (retriable)
    static constexpr uint64 RESULT_MALFORMED          = 4;  ///< key or proof bytes don't parse for this system, or input count mismatch
    // add new result codes above this line
    //
    // A proof that parses but fails verification is NOT a failure code: it is RESULT_CHECKED with valid = 0.

    /// Public inputs per query (each a 32-byte field element, big-endian).
    static constexpr uint64 MAX_PUBLIC_INPUTS = 8;

    /// Largest inline proof; bigger proofs are passed by hash.
    static constexpr uint64 MAX_INLINE_PROOF = 256;

    /// Largest verifying key or by-hash proof an oracle machine will fetch (1 MiB).
    static constexpr uint64 MAX_FETCH_SIZE = 1048576ULL;

    /// Oracle query data / input to the oracle machine. Fixed 808 bytes.
    struct OracleQuery
    {
        /// One of System.
        uint64 system;                       // [0..8)

        /// SHA-256 of the verifying key file. Contracts compare this with the keys they trust.
        Array<uint8, 32> vkSha256;           // [8..40)

        /// Where to fetch the verifying key (a hint; the hash decides).
        uint64 vkLocatorKind;                // [40..48)
        Array<uint8, 64> vkLocator;          // [48..112)

        /// Number of public inputs used (0..MAX_PUBLIC_INPUTS).
        uint64 publicInputCount;             // [112..120)

        /// Public inputs, 32 bytes each, big-endian field elements, zero-padded.
        Array<uint8, 256> publicInputs;      // [120..376)

        /// Inline proof length (0..MAX_INLINE_PROOF). 0 means the proof is given by hash below.
        uint64 proofLen;                     // [376..384)

        /// Inline proof bytes in the system's standard serialization, zero-padded.
        Array<uint8, 256> proof;             // [384..640)

        /// By-hash proof (used when proofLen == 0): SHA-256 of the proof bytes and where to fetch them.
        Array<uint8, 32> proofSha256;        // [640..672)
        uint64 proofLocatorKind;             // [672..680)
        Array<uint8, 64> proofLocator;       // [680..744)

        /// Contract-chosen tag echoed back, e.g. a trade id, so one reply can't be reused for another purpose.
        Array<uint8, 64> context;            // [744..808)
    };

    /// Oracle reply data / output of the oracle machine. Fixed 112 bytes.
    /// On failure (code != RESULT_CHECKED) all other fields MUST be all-zero.
    struct OracleReply
    {
        /// One of the RESULT_* codes above.
        uint64 code;                         // [0..8)

        /// 1 if the proof verified against the key and public inputs, else 0.
        uint64 valid;                        // [8..16)

        /// The verifying key hash that was used (copied from the query).
        Array<uint8, 32> vkSha256;           // [16..48)

        /// The context tag, copied from the query.
        Array<uint8, 64> context;            // [48..112)
    };

    /// Return query fee. Pairing checks are heavier than plain reads.
    static sint64 getQueryFee(const OracleQuery& query)
    {
        return query.proofLen == 0 ? 750 : 500;
    }

    //-------------------------------------------------------------------------
    // Optional: convenience features for contracts using the oracle interface

    /// True if the proof was checked and passed.
    static bool replyIsValid(const OracleReply& reply)
    {
        return reply.code == RESULT_CHECKED && reply.valid == 1;
    }

    /// True if a failed reply should be retried later rather than treated as final.
    static bool replyIsRetriable(const OracleReply& reply)
    {
        return reply.code == RESULT_NOT_FOUND;
    }
};
