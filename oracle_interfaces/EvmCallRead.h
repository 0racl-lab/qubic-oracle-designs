using namespace QPI;

/**
* Oracle interface "EvmCallRead" (see Price.h for general documentation about oracle interfaces).
*
* Generic cross-chain read of EVM contract STATE at a FINALIZED block. Given (chainId, blockNumber,
* to, calldata) the oracle machine runs one read-only eth_call against exactly that block and returns
* the raw return data. Typical calls: balanceOf(address), ownerOf(tokenId), totalSupply(), a reserve
* or price getter. No ABI semantics are baked in: the querying contract encodes the calldata and
* decodes the result itself.
*
* It complements EvmLogRead: EvmLogRead reads that something HAPPENED (an event was emitted),
* EvmCallRead reads what something IS at a fixed block.
*
* Determinism requirement (see Price.h): state at a fixed past block never changes, so the result is
* the same on every machine. The oracle machine MUST
*   - refuse blocks above the chain's finalized head (RESULT_BLOCK_NOT_FINALIZED),
*   - pin the call to the block HASH it resolved (EIP-1898) and return that hash,
*   - call from the zero address with exactly GAS_CAP gas and no state overrides,
*   - zero all reply fields except code on failure.
* A call that reverts is a deterministic outcome: it is returned as RESULT_SUCCESS with reverted = 1
* and the revert data.
*
* Served by oracle-machine oracles/read_evm_call (568-byte query, 568-byte reply).
*/
struct EvmCallRead
{
    //-------------------------------------------------------------------------
    // Mandatory oracle interface definitions

    /// Chain ids (decimal) of supported EVM networks: the same set as EvmLogRead.
    struct ChainId
    {
        static constexpr uint64 ethereum  = 1;       // 0x1
        static constexpr uint64 optimism  = 10;      // 0xa
        static constexpr uint64 bsc       = 56;      // 0x38
        static constexpr uint64 polygon   = 137;     // 0x89
        static constexpr uint64 fantom    = 250;     // 0xfa
        static constexpr uint64 base      = 8453;    // 0x2105
        static constexpr uint64 avalanche = 43114;   // 0xa86a
        static constexpr uint64 arbitrum  = 42161;   // 0xa4b1
        static constexpr uint64 sepolia   = 11155111;// 0xaa36a7 (Ethereum Sepolia testnet)
        // add new chain ids above this line
    };

    /// Return true if the chain id is one this oracle is expected to serve.
    static bool isSupportedChain(uint64 chainId)
    {
        return chainId == ChainId::ethereum
            || chainId == ChainId::optimism
            || chainId == ChainId::bsc
            || chainId == ChainId::polygon
            || chainId == ChainId::fantom
            || chainId == ChainId::base
            || chainId == ChainId::avalanche
            || chainId == ChainId::arbitrum
            || chainId == ChainId::sepolia;
    }

    /// Oracle interface index
    static constexpr uint32 oracleInterfaceIndex = ORACLE_INTERFACE_INDEX;

    //--- Result codes returned in OracleReply.code. 0 means success; any non-zero value is a failure
    //    reason and implies all other reply fields are all-zero.
    static constexpr uint64 RESULT_SUCCESS             = 0;  ///< call executed at the block (it may have reverted, see reverted)
    static constexpr uint64 RESULT_BAD_QUERY           = 1;  ///< malformed query (bad target word, calldataLen outside 4..512, dirty padding)
    static constexpr uint64 RESULT_CHAIN_UNSUPPORTED   = 2;  ///< chainId not served by this oracle
    static constexpr uint64 RESULT_BLOCK_NOT_FINALIZED = 3;  ///< blockNumber above the finalized head
    static constexpr uint64 RESULT_BLOCK_TOO_OLD       = 4;  ///< block older than STATE_WINDOW_SECONDS before the finalized head
    static constexpr uint64 RESULT_OUT_OF_GAS          = 5;  ///< the call needed more than GAS_CAP
    static constexpr uint64 RESULT_RETURN_TOO_LARGE    = 6;  ///< return or revert data longer than 512 bytes
    // add new result codes above this line
    //
    // Code 3 is a time-varying observation: retry later. Codes 3/4 are boundaries that move with the
    // finalized head; near them computors may split and the query just times out.

    /// Gas available to the call. Fixed, so every machine stops at the same point.
    static constexpr uint64 GAS_CAP = 10000000ULL;

    /// Oldest state oracle machines must serve, measured back from the finalized head's timestamp.
    /// About a day: "state at a recent finalized block" without requiring archive nodes.
    static constexpr uint64 STATE_WINDOW_SECONDS = 86400ULL;

    /// Oracle query data / input to the oracle machine. Fixed 568 bytes.
    struct OracleQuery
    {
        /// EVM chain id (decimal), e.g. EvmCallRead::ChainId::base.
        uint64 chainId;              // [0..8)

        /// Block whose state is read. Must be at or below the chain's finalized head.
        uint64 blockNumber;          // [8..16)

        /// Contract to call: 20-byte address right-aligned into a 32-byte word (upper 12 bytes zero).
        Array<uint8, 32> to;         // [16..48)

        /// Number of valid bytes in calldata (4..512).
        uint64 calldataLen;          // [48..56)

        /// ABI-encoded calldata (4-byte selector + arguments); bytes beyond calldataLen MUST be zero.
        Array<uint8, 512> calldata;  // [56..568)
    };

    /// Oracle reply data / output of the oracle machine. Fixed 568 bytes.
    /// On failure (code != RESULT_SUCCESS) all other fields MUST be all-zero.
    struct OracleReply
    {
        /// One of the RESULT_* codes above.
        uint64 code;                 // [0..8)

        /// Hash of the block that was read (32-byte big-endian), tying the answer to one history.
        Array<uint8, 32> blockHash;  // [8..40)

        /// 1 if the call reverted (data then holds the revert data), else 0.
        uint64 reverted;             // [40..48)

        /// Number of valid bytes in data (0..512).
        uint64 dataLen;              // [48..56)

        /// Raw return data (or revert data), zero-padded beyond dataLen.
        Array<uint8, 512> data;      // [56..568)
    };

    /// Return query fee. Cross-chain reads are comparatively expensive (same as EvmLogRead).
    static sint64 getQueryFee(const OracleQuery& query)
    {
        return 1000;
    }

    //-------------------------------------------------------------------------
    // Optional: convenience features for contracts using the oracle interface

    /// True if the call ran at the block and returned normally (did not revert).
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
