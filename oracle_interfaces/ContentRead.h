using namespace QPI;

/**
* Oracle interface "ContentRead" (see Price.h for general documentation about oracle interfaces).
*
* Read a file named by its content hash. The query gives the SHA-256 of the whole file and a
* locator (an IPFS CID, an Arweave transaction id, or nothing). The oracle machine fetches the file
* from the locator or from known mirrors, checks that SHA-256 of the full bytes equals the query's
* hash, and returns the part the contract asked for: the file size, a byte range, or one JSON field.
*
* Determinism (see Price.h): the hash is the truth. Bytes that don't match it are discarded, so every
* oracle machine that finds the file holds exactly the same bytes and gives the same reply. The only
* thing that can differ between machines is whether the file was found in time; that shows up as
* RESULT_NOT_FOUND, which contracts must treat as "not available right now", never as "does not
* exist". A file larger than maxFileSize can't be verified (it is never downloaded in full), so it
* is also RESULT_NOT_FOUND. On failure all reply fields except code MUST be zero.
*
* Served by oracle-machine oracles/read_content (208-byte query, 536-byte reply).
*/
struct ContentRead
{
    //-------------------------------------------------------------------------
    // Mandatory oracle interface definitions

    /// Oracle interface index
    static constexpr uint32 oracleInterfaceIndex = ORACLE_INTERFACE_INDEX;

    //--- Where to look (a hint only: the hash decides what is accepted)
    struct LocatorKind
    {
        static constexpr uint64 none    = 0;    ///< no locator: try the oracle machines' known mirrors only
        static constexpr uint64 ipfsCid = 1;    ///< locator holds an IPFS CID (v0 or v1) as ASCII
        static constexpr uint64 arweave = 2;    ///< locator holds an Arweave transaction id (43 chars, base64url)
        // add new locator kinds above this line
    };

    //--- What to return
    struct Mode
    {
        static constexpr uint64 size      = 1;  ///< only fileSize: proves the file is available now
        static constexpr uint64 byteRange = 2;  ///< `length` bytes from `offset`
        static constexpr uint64 jsonField = 3;  ///< parse the file as JSON and return one value (RFC 8785 canonical form)
        // add new modes above this line
    };

    //--- Result codes returned in OracleReply.code. 0 means success; any non-zero value is a failure
    //    reason and implies all other reply fields are all-zero.
    static constexpr uint64 RESULT_SUCCESS         = 0;  ///< file found and verified; reply fields valid
    static constexpr uint64 RESULT_BAD_QUERY       = 1;  ///< malformed query (zero hash, bad locator, unknown mode, dirty padding, ...)
    static constexpr uint64 RESULT_NOT_FOUND       = 2;  ///< no source delivered bytes matching the hash within maxFileSize (retriable)
    static constexpr uint64 RESULT_BAD_RANGE       = 3;  ///< offset/length outside the file, or length outside 1..MAX_DATA
    static constexpr uint64 RESULT_NOT_JSON        = 4;  ///< mode jsonField but the file is not valid UTF-8 I-JSON (RFC 7493)
    static constexpr uint64 RESULT_NO_FIELD        = 5;  ///< mode jsonField but the path does not exist
    static constexpr uint64 RESULT_VALUE_TOO_LARGE = 6;  ///< the JSON value's canonical form exceeds MAX_DATA bytes
    // add new result codes above this line
    //
    // RESULT_NOT_FOUND is a time-varying observation: retry later, never punish anyone for it.
    // All other codes are properties of the verified bytes and are the same for every machine.

    /// Largest file an oracle machine will fetch (64 MiB).
    static constexpr uint64 MAX_FILE_SIZE = 67108864ULL;

    /// Bytes returned per reply (QPI arrays must have a power-of-two capacity).
    static constexpr uint64 MAX_DATA = 512;

    /// Oracle query data / input to the oracle machine. Fixed 208 bytes.
    struct OracleQuery
    {
        /// SHA-256 of the complete file. This is the truth; the locator is only a hint.
        Array<uint8, 32> sha256;     // [0..32)

        /// One of LocatorKind.
        uint64 locatorKind;          // [32..40)

        /// Number of valid bytes in locator (0 for LocatorKind::none, else 1..64).
        uint64 locatorLen;           // [40..48)

        /// ASCII locator (IPFS CID: base32/base58 alphanumerics; Arweave id: 43 base64url chars),
        /// zero-padded.
        Array<uint8, 64> locator;    // [48..112)

        /// Refuse files larger than this (1..MAX_FILE_SIZE). Also sets the fee.
        uint64 maxFileSize;          // [112..120)

        /// One of Mode.
        uint64 mode;                 // [120..128)

        /// Mode byteRange: first byte to return.
        uint64 offset;               // [128..136)

        /// Mode byteRange: number of bytes to return (1..MAX_DATA).
        uint64 length;               // [136..144)

        /// Mode jsonField: dot path such as "meta.licence" or "items.3.name" (a numeric segment
        /// indexes an array), printable ASCII without '.' inside keys, zero-padded. Empty = the root.
        Array<uint8, 64> jsonPath;   // [144..208)
    };

    /// Oracle reply data / output of the oracle machine. Fixed 536 bytes.
    /// On failure (code != RESULT_SUCCESS) all other fields MUST be all-zero.
    struct OracleReply
    {
        /// One of the RESULT_* codes above.
        uint64 code;                 // [0..8)

        /// Size of the whole verified file in bytes.
        uint64 fileSize;             // [8..16)

        /// Number of valid bytes in data (0..MAX_DATA).
        uint64 dataLen;              // [16..24)

        /// Mode byteRange: the bytes. Mode jsonField: the value's canonical JSON text. Mode size: zero.
        Array<uint8, 512> data;      // [24..536)
    };

    /// Return query fee: 50 QU plus 10 QU per started MiB of maxFileSize, so one query can't make
    /// every oracle machine download a large file cheaply.
    static sint64 getQueryFee(const OracleQuery& query)
    {
        uint64 mib = div(query.maxFileSize + 1048575ULL, 1048576ULL);
        return 50 + 10 * (sint64)mib;
    }

    //-------------------------------------------------------------------------
    // Optional: convenience features for contracts using the oracle interface

    /// True if the reply carries verified content.
    static bool replyIsValid(const OracleReply& reply)
    {
        return reply.code == RESULT_SUCCESS;
    }

    /// True if a failed reply should be retried later rather than treated as final.
    static bool replyIsRetriable(const OracleReply& reply)
    {
        return reply.code == RESULT_NOT_FOUND;
    }
};
