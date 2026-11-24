using namespace QPI;

/**
* Oracle interface "UtxoTxRead" (see Price.h for general documentation about oracle interfaces).
*
* Generic read of ONE output of a confirmed transaction on a Bitcoin-family (UTXO) chain: Dogecoin,
* Bitcoin and Litecoin, plus their test networks. Given (network, txid, outputIndex,
* minConfirmations) the oracle machine returns that output's value in base units (koinu / satoshis /
* litoshis) and its raw scriptPubKey, plus the block it was confirmed in. No address formats or
* semantics are baked in: the querying contract compares the raw script with the one it expects, or
* reads an OP_RETURN output's data itself. It is the UTXO sibling of EvmLogRead.
*
* Determinism requirement (see Price.h): the oracle machine MUST only answer once the transaction's
* block has at least minConfirmations confirmations on EVERY configured node (RESULT_NOT_CONFIRMED
* otherwise), so every honest machine sees the same block and the same output, and MUST zero all
* reply fields except code on failure. Around the confirmation boundary computors may split and the
* query just times out; retrying is the querier's job.
*
* Deliberately NOT returned: whether the output has been spent since. That changes over time and
* would break agreement between machines.
*
* Served by oracle-machine oracles/read_utxo_tx (56-byte query, 208-byte reply). The service reads
* the chain's own node software over its standard JSON-RPC (dogecoind / bitcoind / litecoind with
* txindex=1), which all three chains share.
*/
struct UtxoTxRead
{
    //-------------------------------------------------------------------------
    // Mandatory oracle interface definitions

    /// Networks. 0 is deliberately invalid, so an all-zero query is rejected.
    struct Network
    {
        static constexpr uint64 bitcoin         = 1;
        static constexpr uint64 dogecoin        = 2;
        static constexpr uint64 litecoin        = 3;
        static constexpr uint64 bitcoinTestnet4 = 101;
        static constexpr uint64 dogecoinTestnet = 102;
        static constexpr uint64 litecoinTestnet = 103;
        // add new networks above this line
    };

    /// Return true if the network is one this oracle is expected to serve.
    static bool isSupportedNetwork(uint64 network)
    {
        return network == Network::bitcoin
            || network == Network::dogecoin
            || network == Network::litecoin
            || network == Network::bitcoinTestnet4
            || network == Network::dogecoinTestnet
            || network == Network::litecoinTestnet;
    }

    /// Smallest confirmation depth a query may ask for on a network (0 if unsupported). Faster
    /// chains need more blocks for similar safety: about 30 min on Bitcoin, 10 on Dogecoin, 15 on
    /// Litecoin. Test networks accept 1.
    static uint64 minConfirmationsFor(uint64 network)
    {
        if (network == Network::bitcoin)
        {
            return 3;
        }
        if (network == Network::dogecoin)
        {
            return 10;
        }
        if (network == Network::litecoin)
        {
            return 6;
        }
        if (network == Network::bitcoinTestnet4 || network == Network::dogecoinTestnet || network == Network::litecoinTestnet)
        {
            return 1;
        }
        return 0;
    }

    /// Largest confirmation depth a query may ask for.
    static constexpr uint64 MAX_CONFIRMATIONS = 1000;

    /// Oracle interface index
    static constexpr uint32 oracleInterfaceIndex = ORACLE_INTERFACE_INDEX;

    //--- Result codes returned in OracleReply.code. 0 means success; any non-zero value is a failure
    //    reason and implies all other reply fields are all-zero.
    static constexpr uint64 RESULT_SUCCESS             = 0;  ///< output found under enough confirmations; reply fields valid
    static constexpr uint64 RESULT_BAD_QUERY           = 1;  ///< malformed query (zero txid, minConfirmations out of range)
    static constexpr uint64 RESULT_NETWORK_UNSUPPORTED = 2;  ///< network not served by this oracle
    static constexpr uint64 RESULT_TX_NOT_FOUND        = 3;  ///< no node knows the transaction
    static constexpr uint64 RESULT_NOT_CONFIRMED       = 4;  ///< in the mempool, or fewer than minConfirmations on some node
    static constexpr uint64 RESULT_OUTPUT_OUT_OF_RANGE = 5;  ///< outputIndex >= number of outputs
    static constexpr uint64 RESULT_SCRIPT_TOO_LARGE    = 6;  ///< scriptPubKey longer than 128 bytes
    // add new result codes above this line
    //
    // Codes 3/4 are time-varying observations: treat as retriable, never proof of absence.

    /// Oracle query data / input to the oracle machine. Fixed 56 bytes.
    struct OracleQuery
    {
        /// One of Network.
        uint64 network;              // [0..8)

        /// Transaction id in display order (as block explorers and the node's RPC show it).
        Array<uint8, 32> txid;       // [8..40)

        /// Index of the output (vout) to return.
        uint64 outputIndex;          // [40..48)

        /// Depth required before answering: minConfirmationsFor(network)..MAX_CONFIRMATIONS.
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

        /// Value of the requested output in base units (1 DOGE = 100,000,000 koinu; 1 BTC = 100,000,000 sat).
        uint64 value;                // [64..72)

        /// Number of valid bytes in script (0..128).
        uint64 scriptLen;            // [72..80)

        /// Raw scriptPubKey of the output, zero-padded (P2PKH 25, P2SH 23, P2WPKH 22, P2TR 34, OP_RETURN <= 83).
        Array<uint8, 128> script;    // [80..208)
    };

    /// Return query fee. Cross-chain reads are comparatively expensive (same as EvmLogRead).
    static sint64 getQueryFee(const OracleQuery& query)
    {
        return 1000;
    }

    //-------------------------------------------------------------------------
    // Optional: convenience features for contracts using the oracle interface

    /// True if the query is well-formed for this interface (contracts can check before paying).
    static bool queryIsWellFormed(const OracleQuery& query)
    {
        uint64 minConf = minConfirmationsFor(query.network);
        return minConf != 0 && query.minConfirmations >= minConf && query.minConfirmations <= MAX_CONFIRMATIONS;
    }

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
