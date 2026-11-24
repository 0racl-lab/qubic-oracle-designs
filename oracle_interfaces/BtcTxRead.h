using namespace QPI;

/**
* Oracle interface "BtcTxRead" (see Price.h for general documentation about oracle interfaces).
*
* DESIGN SKETCH - not part of qubic/core. See designs/btc-tx-read.md.
*
* Read ONE output of a confirmed Bitcoin transaction: given (network, txid, outputIndex,
* minConfirmations) the oracle machine returns the output's value in satoshis and its raw
* scriptPubKey, plus the block it was confirmed in. No address formats or semantics are baked in;
* the querying contract compares the raw script with the one it expects (or reads an OP_RETURN
* output's data itself). It is the Bitcoin sibling of EvmLogRead.
*
* Determinism (see Price.h): the oracle machine MUST only answer once the transaction's block is
* buried under at least minConfirmations blocks (RESULT_NOT_CONFIRMED otherwise), so every honest
* machine sees the same block and the same output. Around the confirmation boundary machines may
* split and the query simply times out; retrying is the querier's job. On failure all reply fields
* except code MUST be zero.
*
* Deliberately NOT returned: whether the output has been spent since. That changes over time and
* would break agreement between machines.
*/
struct BtcTxRead
{
    //-------------------------------------------------------------------------
    // Mandatory oracle interface definitions

    /// Oracle interface index
    static constexpr uint32 oracleInterfaceIndex = ORACLE_INTERFACE_INDEX;

    //--- Networks
    struct Network
    {
        static constexpr uint64 mainnet = 0;
        static constexpr uint64 testnet4 = 1;
        static constexpr uint64 signet  = 2;
        // add new networks above this line
    };

    //--- Result codes returned in OracleReply.code. 0 means success; any non-zero value is a failure
    //    reason and implies all other reply fields are all-zero.
    static constexpr uint64 RESULT_SUCCESS              = 0;  ///< output found under enough confirmations
    static constexpr uint64 RESULT_BAD_QUERY            = 1;  ///< malformed query (zero txid, minConfirmations out of range, ...)
    static constexpr uint64 RESULT_NETWORK_UNSUPPORTED  = 2;  ///< network not served
    static constexpr uint64 RESULT_TX_NOT_FOUND         = 3;  ///< no such confirmed transaction (retriable)
    static constexpr uint64 RESULT_NOT_CONFIRMED        = 4;  ///< in the mempool or fewer than minConfirmations (retriable)
    static constexpr uint64 RESULT_OUTPUT_OUT_OF_RANGE  = 5;  ///< outputIndex >= number of outputs
    static constexpr uint64 RESULT_SCRIPT_TOO_LARGE     = 6;  ///< scriptPubKey longer than 128 bytes
    // add new result codes above this line
    //
    // Codes 3/4 are time-varying observations: treat as retriable, never proof of absence.

    /// Smallest and largest confirmation depth a query may ask for.
    static constexpr uint64 MIN_CONFIRMATIONS = 3;
    static constexpr uint64 MAX_CONFIRMATIONS = 100;

    /// Oracle query data / input to the oracle machine. Fixed 56 bytes.
    struct OracleQuery
    {
        /// One of Network.
        uint64 network;              // [0..8)

        /// Transaction id in the usual display order (as shown by block explorers), 32 bytes.
        Array<uint8, 32> txid;       // [8..40)

        /// Index of the output (vout) to return.
        uint64 outputIndex;          // [40..48)

        /// Depth required before answering (MIN_CONFIRMATIONS..MAX_CONFIRMATIONS); 6 is customary.
        uint64 minConfirmations;     // [48..56)
    };

    /// Oracle reply data / output of the oracle machine. Fixed 208 bytes.
    /// On failure (code != RESULT_SUCCESS) all other fields MUST be all-zero.
    struct OracleReply
    {
        /// One of the RESULT_* codes above.
        uint64 code;                 // [0..8)

        /// Hash of the block containing the transaction, display order.
        Array<uint8, 32> blockHash;  // [8..40)

        /// Height of that block.
        uint64 blockHeight;          // [40..48)

        /// That block's header timestamp (Unix seconds).
        uint64 blockTime;            // [48..56)

        /// Number of outputs in the transaction.
        uint64 outputCount;          // [56..64)

        /// Value of the requested output in satoshis.
        uint64 value;                // [64..72)

        /// Number of valid bytes in script (0..128).
        uint64 scriptLen;            // [72..80)

        /// Raw scriptPubKey of the output, zero-padded (P2WPKH 22, P2TR 34, OP_RETURN up to 83 bytes).
        Array<uint8, 128> script;    // [80..208)
    };

    /// Return query fee. Cross-chain reads are comparatively expensive (same as EvmLogRead).
    static sint64 getQueryFee(const OracleQuery& query)
    {
        return 1000;
    }

    //-------------------------------------------------------------------------
    // Optional: convenience features for contracts using the oracle interface

    /// True if the reply carries a usable output.
    static bool replyIsValid(const OracleReply& reply)
    {
        return reply.code == RESULT_SUCCESS;
    }

    /// True if a failed reply should be retried later rather than treated as final.
    static bool replyIsRetriable(const OracleReply& reply)
    {
        return reply.code == RESULT_TX_NOT_FOUND || reply.code == RESULT_NOT_CONFIRMED;
    }
};
