using namespace QPI;

/**
* Oracle interface "EvmTxRead" (see Price.h for general documentation about oracle interfaces).
*
* Generic cross-chain read of ONE FINALIZED EVM TRANSACTION: where and when it was included, who
* sent it, to whom, the native value, and whether it succeeded. Given (chainId, txHash) the oracle
* machine returns the block number, block hash, block timestamp, position in the block, sender,
* recipient, value, nonce, transaction type, status and number of logs.
*
* It is the sibling of EvmLogRead: EvmLogRead returns WHAT a transaction emitted (one raw log),
* EvmTxRead returns WHEN and BY WHOM it happened. Together they let a contract accept a token
* transfer only if it was made after a given moment, which a log alone cannot show. Example:
*   1. EvmLogRead(chainId, txHash, logIndex) -> an ERC-20 Transfer log: token, from, to, amount;
*   2. EvmTxRead(chainId, txHash)            -> blockTimestamp, status;
*   3. the contract checks the timestamp is after the order was opened and status == 1.
* No ABI semantics are baked in.
*
* Determinism requirement (see Price.h): the oracle machine MUST only answer for transactions in
* blocks at or below the chain's finalized head (RESULT_TX_NOT_FINALIZED otherwise), MUST take
* blockTimestamp from the block the receipt names (checking block hash and number agree), and MUST
* zero all reply fields except code on failure, so replies are byte-identical across computors.
* A reverted transaction is a final, deterministic fact: it is returned as RESULT_SUCCESS with
* status = 0, so a contract can tell "failed" apart from "not there yet".
*
* Served by oracle-machine oracles/read_evm_tx (40-byte query, 192-byte reply).
*/
struct EvmTxRead
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
    static constexpr uint64 RESULT_SUCCESS          = 0;  ///< tx found in a finalized block; reply fields valid (check status)
    static constexpr uint64 RESULT_BAD_QUERY        = 1;  ///< malformed query (zero tx hash)
    static constexpr uint64 RESULT_CHAIN_UNSUPPORTED = 2; ///< chainId not served by this oracle
    static constexpr uint64 RESULT_TX_NOT_FOUND     = 3;  ///< no endpoint knows the transaction
    static constexpr uint64 RESULT_TX_NOT_FINALIZED = 4;  ///< pending, or its block is above the finalized head
    // add new result codes above this line
    //
    // Codes 3/4 are time-varying observations: treat as retriable, never proof of permanent absence;
    // near the finality boundary computors may split and the query just times out. Retrying is the
    // job of the querier, not of the oracle machine.

    /// Transaction status values (OracleReply.status), as in the EVM receipt.
    static constexpr uint64 STATUS_REVERTED = 0;
    static constexpr uint64 STATUS_SUCCESS  = 1;

    /// Oracle query data / input to the oracle machine. Fixed 40 bytes.
    struct OracleQuery
    {
        /// EVM chain id (decimal), e.g. EvmTxRead::ChainId::ethereum.
        uint64 chainId;                  // [0..8)

        /// Transaction hash (32-byte big-endian EVM tx hash).
        Array<uint8, 32> txHash;         // [8..40)
    };

    /// Oracle reply data / output of the oracle machine. Fixed 192 bytes.
    /// On failure (code != RESULT_SUCCESS) all other fields MUST be all-zero.
    struct OracleReply
    {
        /// One of the RESULT_* codes above.
        uint64 code;                     // [0..8)

        /// Number of the block that includes the transaction.
        uint64 blockNumber;              // [8..16)

        /// That block's timestamp, in seconds since 1970-01-01 UTC (see getBlockDateAndTime).
        uint64 blockTimestamp;           // [16..24)

        /// Position of the transaction in the block.
        uint64 transactionIndex;         // [24..32)

        /// Hash of that block (32-byte big-endian), tying the answer to one history.
        Array<uint8, 32> blockHash;      // [32..64)

        /// Sender: 20-byte address right-aligned (upper 12 bytes zero).
        Array<uint8, 32> from;           // [64..96)

        /// Recipient: 20-byte address right-aligned; all zero for a contract creation.
        Array<uint8, 32> to;             // [96..128)

        /// Native value sent (wei), uint256 big-endian.
        Array<uint8, 32> value;          // [128..160)

        /// Sender's nonce for this transaction.
        uint64 nonce;                    // [160..168)

        /// STATUS_SUCCESS (1) or STATUS_REVERTED (0).
        uint64 status;                   // [168..176)

        /// EIP-2718 transaction type: 0 legacy, 1 access list, 2 EIP-1559, 3 blob, 4 EIP-7702.
        uint64 txType;                   // [176..184)

        /// Number of logs in the receipt (valid logIndex values for EvmLogRead are 0..logCount-1).
        uint64 logCount;                 // [184..192)
    };

    /// Return query fee. Cross-chain reads are comparatively expensive (same as EvmLogRead).
    static sint64 getQueryFee(const OracleQuery& query)
    {
        return 1000;
    }

    //-------------------------------------------------------------------------
    // Optional: convenience features for contracts using the oracle interface

    /// True if the transaction was found, finalized and succeeded.
    static bool replyIsValid(const OracleReply& reply)
    {
        return reply.code == RESULT_SUCCESS && reply.status == STATUS_SUCCESS;
    }

    /// True if a failed reply should be retried later rather than treated as final.
    static bool replyIsRetriable(const OracleReply& reply)
    {
        return reply.code == RESULT_TX_NOT_FOUND || reply.code == RESULT_TX_NOT_FINALIZED;
    }

    /// Block time as a QPI DateAndTime (UTC), to compare with qpi.now() or a stored DateAndTime.
    /// Returns false if the reply is not a success or the time is out of DateAndTime's range.
    static bool getBlockDateAndTime(const OracleReply& reply, DateAndTime& out)
    {
        out = DateAndTime(1970, 1, 1, 0, 0, 0);
        return reply.code == RESULT_SUCCESS && out.add(0, 0, 0, 0, 0, (sint64)reply.blockTimestamp);
    }
};
