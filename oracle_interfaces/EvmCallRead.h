using namespace QPI;

/**
* Oracle interface "EvmCallRead" (see Price.h for general documentation about oracle interfaces).
*
* DESIGN SKETCH - not part of qubic/core. See designs/evm-call-read.md.
*
* Read EVM contract STATE at a FINALIZED block: given (chainId, blockNumber, to, calldata) the
* oracle machine runs a read-only eth_call against that exact block and returns the raw return
* data. Typical calls: balanceOf(address), ownerOf(tokenId), totalSupply(), a price or reserve
* getter. No ABI semantics are baked in; the querying contract encodes the calldata and decodes the
* result itself. It complements EvmLogRead, which proves that something HAPPENED; this one reads
* what something IS at a fixed moment.
*
* Determinism (see Price.h): state at a fixed past block never changes, so every machine computes
* the same result. The oracle machine MUST refuse blocks above the chain's finalized block
* (RESULT_BLOCK_NOT_FINALIZED), use a fixed gas cap and no state overrides, and MUST zero all reply
* fields except code on failure. A call that reverts is a deterministic outcome and is returned as
* success with reverted = 1 and the revert data.
*/
struct EvmCallRead
{
    //-------------------------------------------------------------------------
    // Mandatory oracle interface definitions

    /// Oracle interface index
    static constexpr uint32 oracleInterfaceIndex = ORACLE_INTERFACE_INDEX;

    // Chain ids: the same numbers as EvmLogRead::ChainId.

    //--- Result codes returned in OracleReply.code. 0 means success; any non-zero value is a failure
    //    reason and implies all other reply fields are all-zero.
    static constexpr uint64 RESULT_SUCCESS             = 0;  ///< call executed (it may still have reverted, see reverted)
    static constexpr uint64 RESULT_BAD_QUERY           = 1;  ///< malformed query (zero target, calldataLen > 512, ...)
    static constexpr uint64 RESULT_CHAIN_UNSUPPORTED   = 2;  ///< chainId not served
    static constexpr uint64 RESULT_BLOCK_NOT_FINALIZED = 3;  ///< blockNumber above the finalized block (retriable)
    static constexpr uint64 RESULT_BLOCK_TOO_OLD       = 4;  ///< older than the state window machines must keep
    static constexpr uint64 RESULT_OUT_OF_GAS          = 5;  ///< the call needed more than GAS_CAP
    static constexpr uint64 RESULT_RETURN_TOO_LARGE    = 6;  ///< return or revert data longer than 512 bytes
    // add new result codes above this line
    //
    // Code 3 is time-varying (retry later). Code 4 is near its boundary only; ask for recent blocks.

    /// Gas available to the call. Fixed so every machine stops at the same point.
    static constexpr uint64 GAS_CAP = 10000000ULL;

    /// How far behind the finalized block machines must still answer (about a day on most chains
    /// is enough for "state at a recent moment"; archive access is not required).
    static constexpr uint64 STATE_WINDOW_SECONDS = 86400ULL;

    /// Oracle query data / input to the oracle machine. Fixed 568 bytes.
    struct OracleQuery
    {
        /// EVM chain id (decimal), e.g. 1 for Ethereum, 8453 for Base.
        uint64 chainId;              // [0..8)

        /// Block whose state to read. Must be at or below the finalized block.
        uint64 blockNumber;          // [8..16)

        /// Contract to call: 20-byte address right-aligned in a 32-byte word (upper 12 bytes zero).
        Array<uint8, 32> to;         // [16..48)

        /// Number of valid bytes in calldata (4..512).
        uint64 calldataLen;          // [48..56)

        /// ABI-encoded calldata (4-byte selector + arguments), zero-padded.
        Array<uint8, 512> calldata;  // [56..568)
    };

    /// Oracle reply data / output of the oracle machine. Fixed 568 bytes.
    /// On failure (code != RESULT_SUCCESS) all other fields MUST be all-zero.
    struct OracleReply
    {
        /// One of the RESULT_* codes above.
        uint64 code;                 // [0..8)

        /// Hash of the block that was read, so the contract can tie the answer to one chain history.
        Array<uint8, 32> blockHash;  // [8..40)

        /// 1 if the call reverted (data then holds the revert data), else 0.
        uint64 reverted;             // [40..48)

        /// Number of valid bytes in data (0..512).
        uint64 dataLen;              // [48..56)

        /// Raw return data (or revert data), zero-padded.
        Array<uint8, 512> data;      // [56..568)
    };

    /// Return query fee. Cross-chain reads are comparatively expensive (same as EvmLogRead).
    static sint64 getQueryFee(const OracleQuery& query)
    {
        return 1000;
    }

    //-------------------------------------------------------------------------
    // Optional: convenience features for contracts using the oracle interface

    /// True if the call ran and returned normally.
    static bool replyIsValid(const OracleReply& reply)
    {
        return reply.code == RESULT_SUCCESS && reply.reverted == 0;
    }

    /// True if a failed reply should be retried later rather than treated as final.
    static bool replyIsRetriable(const OracleReply& reply)
    {
        return reply.code == RESULT_BLOCK_NOT_FINALIZED;
    }
};
