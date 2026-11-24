using namespace QPI;

/**
* Oracle interface "HttpRead" (see Price.h for general documentation about oracle interfaces).
*
* DRAFT - a plan for discussion, not implemented. See designs/http-read.md.
*
* Read ONE value from an approved public web API. There are no raw URLs: a query names a source and
* one of its endpoints, both defined in reviewed code (an adapter in the oracle machine, listed here),
* plus validated parameters, an exact timestamp, a field path and how to return the value. Like
* Price, each answer is pinned to a moment, so every machine asks the same question.
*
* Determinism (see Price.h): only endpoints whose answer cannot change are allowed in phase 1:
*   - historical values for a past timestamp (a daily FX reference rate, a day's weather record), or
*   - final published figures, where the adapter checks the source's own "final" flag and answers
*     RESULT_NOT_FINAL until then (retriable).
* Numbers are taken from the response TEXT as exact decimals, scaled by 10^decimals and rounded
* half-to-even, never via floating point. On failure all reply fields except code MUST be zero. A
* source that errors or that disagrees with the machine's second fetch makes the machine abstain.
*/
struct HttpRead
{
    //-------------------------------------------------------------------------
    // Mandatory oracle interface definitions

    /// Oracle interface index
    static constexpr uint32 oracleInterfaceIndex = ORACLE_INTERFACE_INDEX;

    //--- How the value is returned
    struct ValueType
    {
        static constexpr uint64 decimal = 1;  ///< number scaled by 10^decimals into intValue (round half to even)
        static constexpr uint64 text    = 2;  ///< UTF-8 string into text (up to 128 bytes)
        static constexpr uint64 boolean = 3;  ///< true/false into intValue (1/0)
    };

    //--- Result codes returned in OracleReply.code. 0 means success; any non-zero value is a failure
    //    reason and implies all other reply fields are all-zero.
    static constexpr uint64 RESULT_SUCCESS            = 0;  ///< value returned
    static constexpr uint64 RESULT_BAD_QUERY          = 1;  ///< parameters fail the endpoint's validation, bad timestamp, dirty padding
    static constexpr uint64 RESULT_SOURCE_UNSUPPORTED = 2;  ///< unknown source or endpoint
    static constexpr uint64 RESULT_NO_DATA            = 3;  ///< the source definitively has no value for these parameters and time
    static constexpr uint64 RESULT_NOT_FINAL          = 4;  ///< the event or period is not final yet (retriable)
    static constexpr uint64 RESULT_NO_FIELD           = 5;  ///< the field path is not in the response
    static constexpr uint64 RESULT_TYPE_MISMATCH      = 6;  ///< the field is not of the requested value type
    static constexpr uint64 RESULT_VALUE_TOO_LARGE    = 7;  ///< scaled number outside sint64, or text over 128 bytes
    // add new result codes above this line

    /// Largest scaling exponent for ValueType::decimal.
    static constexpr uint64 MAX_DECIMALS = 18;

    /// Oracle query data / input to the oracle machine. Fixed 256 bytes.
    struct OracleQuery
    {
        /// Source, e.g. getFrankfurterSourceId(). Each source is one reviewed adapter.
        id source;                       // [0..32)

        /// Endpoint number within the source (documented per source in designs/http-read.md).
        uint64 endpoint;                 // [32..40)

        /// The moment the value is for (historical endpoints), or the event's scheduled time.
        DateAndTime timestamp;           // [40..48)

        /// Up to 4 parameters, 32 bytes each, printable ASCII, zero-padded; each validated by the
        /// endpoint's own rule (e.g. a currency code, a coordinate, an event id).
        Array<uint8, 128> params;        // [48..176)

        /// Dot path of the field to return within the endpoint's normalized response, zero-padded.
        Array<uint8, 64> field;          // [176..240)

        /// One of ValueType.
        uint64 valueType;                // [240..248)

        /// ValueType::decimal: scaling exponent 0..MAX_DECIMALS; otherwise 0.
        uint64 decimals;                 // [248..256)
    };

    /// Oracle reply data / output of the oracle machine. Fixed 160 bytes.
    /// On failure (code != RESULT_SUCCESS) all other fields MUST be all-zero.
    struct OracleReply
    {
        /// One of the RESULT_* codes above.
        uint64 code;                     // [0..8)

        /// The value type returned (copied from the query).
        uint64 valueType;                // [8..16)

        /// decimal: the scaled number; boolean: 1 or 0; text: 0.
        sint64 intValue;                 // [16..24)

        /// text: number of valid bytes in text (0..128); otherwise 0.
        uint64 textLen;                  // [24..32)

        /// text: UTF-8 bytes, zero-padded.
        Array<uint8, 128> text;          // [32..160)
    };

    /// Return query fee. Web reads cost oracle machines bandwidth and the source's rate limits.
    static sint64 getQueryFee(const OracleQuery& query)
    {
        return 200;
    }

    //-------------------------------------------------------------------------
    // Optional: convenience features for contracts using the oracle interface

    /// Source: Frankfurter (European Central Bank reference exchange rates, daily, historical).
    static id getFrankfurterSourceId()
    {
        using namespace Ch;
        return id(f, r, a, n, k, f, u, r, t, e, r);
    }

    /// Source: Open-Meteo historical weather archive (daily values by coordinate and date).
    static id getOpenMeteoArchiveSourceId()
    {
        using namespace Ch;
        return id(o, p, e, n, m, e, t, e, o, underscore, a, r);
    }

    /// True if the reply carries a value.
    static bool replyIsValid(const OracleReply& reply)
    {
        return reply.code == RESULT_SUCCESS;
    }

    /// True if a failed reply should be retried later rather than treated as final.
    static bool replyIsRetriable(const OracleReply& reply)
    {
        return reply.code == RESULT_NOT_FINAL;
    }
};
