using namespace QPI;

// ============================================================================
// FeedRegistry (contract FEEDREG) - web data feeds for Qubic contracts,
// voted in by the registry's shareholders and read through the HttpRead oracle.
//
// DESIGN SKETCH, written as if HttpRead (oracle_interfaces/HttpRead.h) were
// live. Not proposed to qubic/core. See designs/http-read.md, "Feeds governed
// by a registry contract".
//
// What it does:
//   - Stores feed definitions: an https URL template, typed parameters, which
//     JSON value to return and how old the data must be. Anyone can propose one
//     with a deposit. Shareholders vote it in. Definitions never change.
//   - Is the only way to read a feed. A contract (or a user) calls RequestValue
//     with a feed id, a timestamp and the parameters. The registry checks them,
//     takes the feed's fee and sends an HttpRead query for the feedreg source.
//     The reply is stored and read back with GetRequest.
//
// Why requests go through the registry: an oracle machine is not told which
// contract sent a query. Here the registry sends every feed query itself and
// records the oracle query id with the request. For the feedreg source, an
// oracle machine reads that record (GetRequest, public state) and serves the
// query only if the record exists, is pending, carries this very query id and
// matches the query's fields, and the feed's stored definition hashes to the
// recorded definitionHash. A query sent directly, bypassing the registry and
// its fee, has no record and is refused. Restrictions and fees are enforced
// here, where the caller is known (qpi.invocator()).
//
// Contracts can only call contracts with a lower index, so consumers must be
// deployed after the registry. They call RequestValue and later read
// GetRequest; the registry cannot call them back.
//
// Lifecycle of a feed:
//   ProposeFeed (deposit) -> PENDING
//   PENDING -- shareholder vote "approve feedId" accepted at END_EPOCH --> APPROVED, deposit refunded
//   PENDING -- no accepted vote within FEEDREG_PENDING_EPOCHS epochs --> EXPIRED, deposit burned
//   PENDING -- WithdrawFeed by the proposer --> WITHDRAWN, half the deposit refunded, half burned
//   APPROVED -- shareholder vote "retire feedId" accepted --> RETIRED
// Feed ids are never reused.
//
// Money: the requester pays the HttpRead query fee (paid on to the oracle as
// usual) plus the feed's feePerUse. The feed fee is held until the oracle
// answers: on an answer the proposer gets proposerShareBps of it and the rest
// is paid to shareholders as dividends at END_EPOCH; if the oracle times out
// or cannot agree, the feed fee is refunded. The oracle fee is not refundable.
//
// Shareholder proposals (yes/no, applied at END_EPOCH):
//   variable 0  approve feed <value = feedId>     (feed must be PENDING)
//   variable 1  retire feed <value = feedId>      (feed must be APPROVED)
//   variable 2  propose deposit in QU             (10^7 .. 10^13)
//   variable 3  proposer share in basis points    (0 .. 5000)
//   variable 4  shareholder proposal fee in QU    (0 .. 10^10)
// ============================================================================

constexpr uint64 FEEDREG_CONTRACT_ASSET_NAME = 20060942981481798;   // "FEEDREG"
constexpr uint64 FEEDREG_MAX_FEEDS = 1024;
constexpr uint64 FEEDREG_MAX_REQUESTS = 1024;      // ring of recent requests; a slot is reused only once answered
constexpr uint32 FEEDREG_PROPOSAL_SLOTS = 16;
constexpr uint16 FEEDREG_PENDING_EPOCHS = 4;
constexpr uint32 FEEDREG_LIST_PAGE = 16;
constexpr uint32 FEEDREG_ORACLE_TIMEOUT_MS = 120000;

// A feed is a standing claim on every oracle machine's bandwidth and on a third
// party's API, so proposing one must cost real money. The deposit comes back
// when the feed is approved; it is burned if nobody votes for it.
constexpr sint64 FEEDREG_DEFAULT_DEPOSIT = 1000000000;
constexpr sint64 FEEDREG_DEFAULT_PROPOSER_SHARE_BPS = 2000;
constexpr sint64 FEEDREG_DEFAULT_PROPOSAL_FEE = 1000000;
constexpr sint64 FEEDREG_MIN_DEPOSIT = 10000000;
constexpr sint64 FEEDREG_MAX_DEPOSIT = 10000000000000;
constexpr sint64 FEEDREG_MAX_PROPOSER_SHARE_BPS = 5000;
constexpr sint64 FEEDREG_MAX_PROPOSAL_FEE = 10000000000;
constexpr sint64 FEEDREG_MAX_FEE_PER_USE = 10000000000;

// minimum age of the requested moment: HISTORICAL 1 minute .. 31 days, FINAL 0 .. 31 days
constexpr uint32 FEEDREG_MIN_AGE_SECONDS = 60;
constexpr uint32 FEEDREG_MAX_AGE_SECONDS = 2678400;

// shareholder proposal variables
constexpr uint64 FEEDREG_VAR_APPROVE_FEED = 0;
constexpr uint64 FEEDREG_VAR_RETIRE_FEED = 1;
constexpr uint64 FEEDREG_VAR_DEPOSIT = 2;
constexpr uint64 FEEDREG_VAR_PROPOSER_SHARE_BPS = 3;
constexpr uint64 FEEDREG_VAR_PROPOSAL_FEE = 4;
constexpr uint64 FEEDREG_VARIABLE_COUNT = 5;

// feed status
constexpr uint32 FEEDREG_STATUS_NONE = 0;
constexpr uint32 FEEDREG_STATUS_PENDING = 1;
constexpr uint32 FEEDREG_STATUS_APPROVED = 2;
constexpr uint32 FEEDREG_STATUS_RETIRED = 3;
constexpr uint32 FEEDREG_STATUS_EXPIRED = 4;
constexpr uint32 FEEDREG_STATUS_WITHDRAWN = 5;

// request status
constexpr uint32 FEEDREG_REQUEST_NONE = 0;
constexpr uint32 FEEDREG_REQUEST_PENDING = 1;
constexpr uint32 FEEDREG_REQUEST_ANSWERED = 2;       // reply holds HttpRead's answer (its code may still be NO_DATA etc.)
constexpr uint32 FEEDREG_REQUEST_NOT_ANSWERED = 3;   // oracle timed out or could not agree; feed fee refunded

// parameter kinds; checked here when a value is requested, and again by the oracle machine
constexpr uint8 FEEDREG_PARAM_NONE = 0;
constexpr uint8 FEEDREG_PARAM_ISO_DATE = 1;        // YYYY-MM-DD, a real calendar date
constexpr uint8 FEEDREG_PARAM_CURRENCY = 2;        // 3 upper-case letters
constexpr uint8 FEEDREG_PARAM_INTEGER = 3;         // -?[0-9]{1,18}
constexpr uint8 FEEDREG_PARAM_DECIMAL = 4;         // -?[0-9]{1,18}(\.[0-9]{1,18})?
constexpr uint8 FEEDREG_PARAM_SLUG = 5;            // [a-z0-9-]{1,32}
constexpr uint8 FEEDREG_PARAM_COORDINATE = 6;      // -?[0-9]{1,3}(\.[0-9]{1,4})?, at most 180
constexpr uint8 FEEDREG_PARAM_KIND_COUNT = 7;

// time policy
constexpr uint8 FEEDREG_TIME_HISTORICAL = 1;       // values for a moment at least minAgeSeconds ago
constexpr uint8 FEEDREG_TIME_FINAL = 2;            // the source marks the record final at finalPath

// return codes
constexpr uint32 FEEDREG_OK = 0;
constexpr uint32 FEEDREG_ERR_DEPOSIT = 1;
constexpr uint32 FEEDREG_ERR_FULL = 2;
constexpr uint32 FEEDREG_ERR_DUPLICATE = 3;
constexpr uint32 FEEDREG_ERR_URL_SCHEME = 4;
constexpr uint32 FEEDREG_ERR_URL_HOST = 5;
constexpr uint32 FEEDREG_ERR_URL_PATH = 6;
constexpr uint32 FEEDREG_ERR_URL_PARAMS = 7;
constexpr uint32 FEEDREG_ERR_PARAM_KINDS = 8;
constexpr uint32 FEEDREG_ERR_TIME_POLICY = 9;
constexpr uint32 FEEDREG_ERR_VALUE_PATH = 10;
constexpr uint32 FEEDREG_ERR_FINAL_PATH = 11;
constexpr uint32 FEEDREG_ERR_VALUE_TYPE = 12;
constexpr uint32 FEEDREG_ERR_FEE = 13;
constexpr uint32 FEEDREG_ERR_RESERVED = 14;
constexpr uint32 FEEDREG_ERR_NOT_FOUND = 15;
constexpr uint32 FEEDREG_ERR_NOT_PROPOSER = 16;
constexpr uint32 FEEDREG_ERR_WRONG_STATUS = 17;
constexpr uint32 FEEDREG_ERR_RESTRICTED = 18;
constexpr uint32 FEEDREG_ERR_TIMESTAMP = 19;       // not a valid date and time
constexpr uint32 FEEDREG_ERR_TOO_RECENT = 20;      // later than now minus the feed's minimum age
constexpr uint32 FEEDREG_ERR_PARAMS = 21;          // a parameter fails its kind, or an unused one is not zero
constexpr uint32 FEEDREG_ERR_PAYMENT = 22;
constexpr uint32 FEEDREG_ERR_BUSY = 23;            // the request ring is full of unanswered requests
constexpr uint32 FEEDREG_ERR_ORACLE_REFUSED = 24;  // QUERY_ORACLE failed

// ASCII codes (contract code may not use character literals)
constexpr uint8 FEEDREG_CH_H = 104;
constexpr uint8 FEEDREG_CH_T = 116;
constexpr uint8 FEEDREG_CH_P = 112;
constexpr uint8 FEEDREG_CH_S = 115;
constexpr uint8 FEEDREG_CH_COLON = 58;
constexpr uint8 FEEDREG_CH_SLASH = 47;
constexpr uint8 FEEDREG_CH_QUESTION = 63;
constexpr uint8 FEEDREG_CH_DOT = 46;
constexpr uint8 FEEDREG_CH_DASH = 45;
constexpr uint8 FEEDREG_CH_UNDERSCORE = 95;
constexpr uint8 FEEDREG_CH_TILDE = 126;
constexpr uint8 FEEDREG_CH_AMP = 38;
constexpr uint8 FEEDREG_CH_EQUALS = 61;
constexpr uint8 FEEDREG_CH_COMMA = 44;
constexpr uint8 FEEDREG_CH_PLUS = 43;
constexpr uint8 FEEDREG_CH_PERCENT = 37;
constexpr uint8 FEEDREG_CH_LBRACE = 123;
constexpr uint8 FEEDREG_CH_RBRACE = 125;
constexpr uint8 FEEDREG_CH_0 = 48;
constexpr uint8 FEEDREG_CH_1 = 49;
constexpr uint8 FEEDREG_CH_9 = 57;
constexpr uint8 FEEDREG_CH_UPPER_A = 65;
constexpr uint8 FEEDREG_CH_UPPER_F = 70;
constexpr uint8 FEEDREG_CH_UPPER_Z = 90;
constexpr uint8 FEEDREG_CH_LOWER_A = 97;
constexpr uint8 FEEDREG_CH_LOWER_F = 102;
constexpr uint8 FEEDREG_CH_LOWER_Z = 122;

struct FEEDREG2
{
};

struct FEEDREG : public ContractBase
{
public:
    DEFINE_SHAREHOLDER_PROPOSAL_TYPES(FEEDREG_PROPOSAL_SLOTS, FEEDREG_CONTRACT_ASSET_NAME);

    // What a feed means. Hashed with K12 into definitionHash; every byte counts,
    // so unused bytes must be zero (checked).
    struct FeedDefinition
    {
        Array<uint8, 256> urlTemplate;   // https://host/path?query with {0}..{3}; zero padded; byte 255 zero
        Array<uint8, 64> valuePath;      // JSON pointer to the value, may contain {0}..{3}; zero padded
        Array<uint8, 64> finalPath;      // FINAL feeds: JSON pointer to a boolean that must be true; else all zero
        Array<uint8, 4> paramKinds;      // FEEDREG_PARAM_*, leading entries used, rest zero
        uint32 minAgeSeconds;            // the requested moment must be at least this old
        uint8 timePolicy;                // FEEDREG_TIME_*
        uint8 valueType;                 // OI::HttpRead::ValueType
        uint8 decimals;                  // decimal: 0..18 (0 = whole numbers); text, boolean: 0
        uint8 reserved;                  // must be zero
    };

    struct Feed
    {
        FeedDefinition definition;
        id definitionHash;
        id proposer;
        id restrictedTo;                 // NULL_ID: any caller may request values
        sint64 feePerUse;                // charged per answered request, on top of the oracle fee
        sint64 deposit;                  // held while PENDING
        uint64 requestCount;             // answered requests
        uint32 status;
        uint16 proposedEpoch;
        uint16 decidedEpoch;
    };

    // One value request. The oracle machine serving a feedreg query reads this record.
    struct Request
    {
        uint64 requestId;                // 0: empty slot
        uint64 feedId;
        id requester;
        id definitionHash;               // of the feed when requested
        DateAndTime timestamp;
        Array<uint8, 128> params;
        sint64 oracleQueryId;            // the HttpRead query sent for this request
        sint64 feeHeld;                  // feed fee held until the oracle answers
        uint32 status;                   // FEEDREG_REQUEST_*
        uint32 requestTick;
        uint32 answerTick;
        uint32 reserved;
        OI::HttpRead::OracleReply reply; // valid when status is ANSWERED
    };

    struct StateData
    {
        ProposalVotingT proposals;
        Array<Feed, FEEDREG_MAX_FEEDS> feeds;                  // feedId = index + 1
        Array<Request, FEEDREG_MAX_REQUESTS> requests;         // slot = requestId mod FEEDREG_MAX_REQUESTS
        HashMap<id, uint64, FEEDREG_MAX_FEEDS> liveHashes;     // definitionHash -> feedId while PENDING or APPROVED
        HashMap<sint64, uint64, FEEDREG_MAX_REQUESTS> queryToRequest;  // pending oracle query id -> requestId
        uint64 feedCount;
        uint64 nextRequestId;
        sint64 proposeDeposit;
        sint64 proposerShareBps;
        sint64 proposalFee;
        sint64 heldDeposits;
        sint64 heldFees;
        sint64 dividendPool;
        sint64 totalPaidToShareholders;
        sint64 totalPaidToProposers;
    };

    // ------------------------------------------------------------------ validation helpers

    struct IsPathChar_input { uint8 c; };
    struct IsPathChar_output { bit ok; };
    // unreserved characters of RFC 3986 except '~' (handled by callers)
    PRIVATE_FUNCTION(IsPathChar)
    {
        output.ok = (input.c >= FEEDREG_CH_LOWER_A && input.c <= FEEDREG_CH_LOWER_Z)
            || (input.c >= FEEDREG_CH_UPPER_A && input.c <= FEEDREG_CH_UPPER_Z)
            || (input.c >= FEEDREG_CH_0 && input.c <= FEEDREG_CH_9)
            || input.c == FEEDREG_CH_DASH || input.c == FEEDREG_CH_DOT || input.c == FEEDREG_CH_UNDERSCORE;
    }

    struct IsHex_input { uint8 c; };
    struct IsHex_output { bit ok; };
    PRIVATE_FUNCTION(IsHex)
    {
        output.ok = (input.c >= FEEDREG_CH_0 && input.c <= FEEDREG_CH_9)
            || (input.c >= FEEDREG_CH_UPPER_A && input.c <= FEEDREG_CH_UPPER_F)
            || (input.c >= FEEDREG_CH_LOWER_A && input.c <= FEEDREG_CH_LOWER_F);
    }

    struct ValidateParamKinds_input { Array<uint8, 4> kinds; };
    struct ValidateParamKinds_output { uint32 returnCode; uint8 count; };
    struct ValidateParamKinds_locals { uint32 i; bit ended; uint8 k; };
    PRIVATE_FUNCTION_WITH_LOCALS(ValidateParamKinds)
    {
        output.returnCode = FEEDREG_OK;
        output.count = 0;
        locals.ended = false;
        for (locals.i = 0; locals.i < 4; locals.i++)
        {
            locals.k = input.kinds.get(locals.i);
            if (locals.k == FEEDREG_PARAM_NONE)
            {
                locals.ended = true;
                continue;
            }
            if (locals.ended || locals.k >= FEEDREG_PARAM_KIND_COUNT)
            {
                output.returnCode = FEEDREG_ERR_PARAM_KINDS;
                return;
            }
            output.count++;
        }
    }

    // https://host/path?query. Host: lower-case DNS name with at least one dot,
    // top-level label letters only (so no IP literals), no port, no userinfo.
    // Path and query: unreserved characters, / ? & = , : + ~, %XX escapes and
    // placeholders {0}..{3}; each declared parameter must be used.
    struct ValidateUrl_input { Array<uint8, 256> url; uint8 paramCount; };
    struct ValidateUrl_output { uint32 returnCode; };
    struct ValidateUrl_locals
    {
        uint32 i;
        uint32 hostEnd;
        uint32 lastDot;
        uint32 dots;
        uint8 c;
        uint8 prev;
        uint8 used;
        uint8 d;
        bit ended;
        IsPathChar_input pc;
        IsPathChar_output pcOut;
        IsHex_input hx;
        IsHex_output hxOut;
    };
    PRIVATE_FUNCTION_WITH_LOCALS(ValidateUrl)
    {
        output.returnCode = FEEDREG_ERR_URL_SCHEME;
        if (input.url.get(0) != FEEDREG_CH_H || input.url.get(1) != FEEDREG_CH_T || input.url.get(2) != FEEDREG_CH_T
            || input.url.get(3) != FEEDREG_CH_P || input.url.get(4) != FEEDREG_CH_S || input.url.get(5) != FEEDREG_CH_COLON
            || input.url.get(6) != FEEDREG_CH_SLASH || input.url.get(7) != FEEDREG_CH_SLASH)
        {
            return;
        }

        // host
        output.returnCode = FEEDREG_ERR_URL_HOST;
        locals.dots = 0;
        locals.lastDot = 8;
        locals.prev = 0;
        for (locals.i = 8; locals.i < 256; locals.i++)
        {
            locals.c = input.url.get(locals.i);
            if (locals.c == 0 || locals.c == FEEDREG_CH_SLASH || locals.c == FEEDREG_CH_QUESTION)
            {
                break;
            }
            if (locals.c == FEEDREG_CH_DOT)
            {
                if (locals.i == 8 || locals.prev == FEEDREG_CH_DOT || locals.prev == FEEDREG_CH_DASH)
                {
                    return;
                }
                locals.dots++;
                locals.lastDot = locals.i;
            }
            else if (locals.c == FEEDREG_CH_DASH)
            {
                if (locals.i == 8 || locals.prev == FEEDREG_CH_DOT)
                {
                    return;
                }
            }
            else if (!((locals.c >= FEEDREG_CH_LOWER_A && locals.c <= FEEDREG_CH_LOWER_Z) || (locals.c >= FEEDREG_CH_0 && locals.c <= FEEDREG_CH_9)))
            {
                return;
            }
            locals.prev = locals.c;
        }
        locals.hostEnd = locals.i;
        // the last label must be at least 2 letters; that also rules out a trailing dot or dash
        if (locals.dots == 0 || locals.hostEnd - locals.lastDot < 3)
        {
            return;
        }
        for (locals.i = locals.lastDot + 1; locals.i < locals.hostEnd; locals.i++)
        {
            locals.c = input.url.get(locals.i);
            if (locals.c < FEEDREG_CH_LOWER_A || locals.c > FEEDREG_CH_LOWER_Z)
            {
                return;
            }
        }

        // path and query
        output.returnCode = FEEDREG_ERR_URL_PATH;
        if (locals.hostEnd >= 255 || input.url.get(locals.hostEnd) != FEEDREG_CH_SLASH || input.url.get(255) != 0)
        {
            return;
        }
        locals.used = 0;
        locals.ended = false;
        for (locals.i = locals.hostEnd; locals.i < 256; locals.i++)
        {
            locals.c = input.url.get(locals.i);
            if (locals.ended)
            {
                if (locals.c != 0)
                {
                    return;
                }
                continue;
            }
            if (locals.c == 0)
            {
                locals.ended = true;
                continue;
            }
            locals.pc.c = locals.c;
            CALL(IsPathChar, locals.pc, locals.pcOut);
            if (locals.pcOut.ok || locals.c == FEEDREG_CH_SLASH || locals.c == FEEDREG_CH_QUESTION || locals.c == FEEDREG_CH_AMP
                || locals.c == FEEDREG_CH_EQUALS || locals.c == FEEDREG_CH_COMMA || locals.c == FEEDREG_CH_COLON
                || locals.c == FEEDREG_CH_PLUS || locals.c == FEEDREG_CH_TILDE)
            {
                continue;
            }
            if (locals.c == FEEDREG_CH_PERCENT)
            {
                // byte 255 is zero (checked above), so an escape cut off by the end fails the hex test
                locals.hx.c = input.url.get(locals.i + 1);
                CALL(IsHex, locals.hx, locals.hxOut);
                if (!locals.hxOut.ok)
                {
                    return;
                }
                locals.hx.c = input.url.get(locals.i + 2);
                CALL(IsHex, locals.hx, locals.hxOut);
                if (!locals.hxOut.ok)
                {
                    return;
                }
                locals.i += 2;
                continue;
            }
            if (locals.c == FEEDREG_CH_LBRACE)
            {
                if (locals.i + 2 > 254 || input.url.get(locals.i + 2) != FEEDREG_CH_RBRACE)
                {
                    return;
                }
                locals.d = input.url.get(locals.i + 1);
                if (locals.d < FEEDREG_CH_0 || locals.d - FEEDREG_CH_0 >= input.paramCount)
                {
                    output.returnCode = FEEDREG_ERR_URL_PARAMS;
                    return;
                }
                locals.used |= (uint8)(1 << (locals.d - FEEDREG_CH_0));
                locals.i += 2;
                continue;
            }
            return;
        }
        if (locals.used != (uint8)((1 << input.paramCount) - 1))
        {
            output.returnCode = FEEDREG_ERR_URL_PARAMS;
            return;
        }
        output.returnCode = FEEDREG_OK;
    }

    // JSON pointer (RFC 6901) of at most 63 bytes, zero padded: "/" then
    // unreserved characters, "/", "~0", "~1" and placeholders {0}..{3}.
    struct ValidatePath_input { Array<uint8, 64> path; uint8 paramCount; };
    struct ValidatePath_output { bit ok; bit empty; };
    struct ValidatePath_locals
    {
        uint32 i;
        uint8 c;
        uint8 d;
        bit ended;
        IsPathChar_input pc;
        IsPathChar_output pcOut;
    };
    PRIVATE_FUNCTION_WITH_LOCALS(ValidatePath)
    {
        output.ok = false;
        output.empty = true;
        for (locals.i = 0; locals.i < 64; locals.i++)
        {
            if (input.path.get(locals.i) != 0)
            {
                output.empty = false;
            }
        }
        if (output.empty)
        {
            output.ok = true;
            return;
        }
        if (input.path.get(0) != FEEDREG_CH_SLASH || input.path.get(63) != 0)
        {
            return;
        }
        locals.ended = false;
        for (locals.i = 1; locals.i < 64; locals.i++)
        {
            locals.c = input.path.get(locals.i);
            if (locals.ended)
            {
                if (locals.c != 0)
                {
                    return;
                }
                continue;
            }
            if (locals.c == 0)
            {
                locals.ended = true;
                continue;
            }
            locals.pc.c = locals.c;
            CALL(IsPathChar, locals.pc, locals.pcOut);
            if (locals.pcOut.ok || locals.c == FEEDREG_CH_SLASH)
            {
                continue;
            }
            if (locals.c == FEEDREG_CH_TILDE)
            {
                locals.d = input.path.get(locals.i + 1);
                if (locals.d != FEEDREG_CH_0 && locals.d != FEEDREG_CH_1)
                {
                    return;
                }
                locals.i += 1;
                continue;
            }
            if (locals.c == FEEDREG_CH_LBRACE)
            {
                if (locals.i + 2 > 62 || input.path.get(locals.i + 2) != FEEDREG_CH_RBRACE)
                {
                    return;
                }
                locals.d = input.path.get(locals.i + 1);
                if (locals.d < FEEDREG_CH_0 || locals.d - FEEDREG_CH_0 >= input.paramCount)
                {
                    return;
                }
                locals.i += 2;
                continue;
            }
            return;
        }
        output.ok = true;
    }

    struct ValidateDefinition_input { FeedDefinition definition; };
    struct ValidateDefinition_output { uint32 returnCode; };
    struct ValidateDefinition_locals
    {
        ValidateParamKinds_input kinds;
        ValidateParamKinds_output kindsOut;
        ValidateUrl_input url;
        ValidateUrl_output urlOut;
        ValidatePath_input path;
        ValidatePath_output pathOut;
    };
    PRIVATE_FUNCTION_WITH_LOCALS(ValidateDefinition)
    {
        if (input.definition.reserved != 0)
        {
            output.returnCode = FEEDREG_ERR_RESERVED;
            return;
        }

        locals.kinds.kinds = input.definition.paramKinds;
        CALL(ValidateParamKinds, locals.kinds, locals.kindsOut);
        if (locals.kindsOut.returnCode != FEEDREG_OK)
        {
            output.returnCode = locals.kindsOut.returnCode;
            return;
        }

        locals.url.url = input.definition.urlTemplate;
        locals.url.paramCount = locals.kindsOut.count;
        CALL(ValidateUrl, locals.url, locals.urlOut);
        if (locals.urlOut.returnCode != FEEDREG_OK)
        {
            output.returnCode = locals.urlOut.returnCode;
            return;
        }

        locals.path.path = input.definition.valuePath;
        locals.path.paramCount = locals.kindsOut.count;
        CALL(ValidatePath, locals.path, locals.pathOut);
        if (!locals.pathOut.ok || locals.pathOut.empty)
        {
            output.returnCode = FEEDREG_ERR_VALUE_PATH;
            return;
        }

        locals.path.path = input.definition.finalPath;
        CALL(ValidatePath, locals.path, locals.pathOut);
        if (!locals.pathOut.ok)
        {
            output.returnCode = FEEDREG_ERR_FINAL_PATH;
            return;
        }
        if (input.definition.timePolicy == FEEDREG_TIME_HISTORICAL)
        {
            if (!locals.pathOut.empty)
            {
                output.returnCode = FEEDREG_ERR_FINAL_PATH;
                return;
            }
            if (input.definition.minAgeSeconds < FEEDREG_MIN_AGE_SECONDS || input.definition.minAgeSeconds > FEEDREG_MAX_AGE_SECONDS)
            {
                output.returnCode = FEEDREG_ERR_TIME_POLICY;
                return;
            }
        }
        else if (input.definition.timePolicy == FEEDREG_TIME_FINAL)
        {
            if (locals.pathOut.empty)
            {
                output.returnCode = FEEDREG_ERR_FINAL_PATH;
                return;
            }
            if (input.definition.minAgeSeconds > FEEDREG_MAX_AGE_SECONDS)
            {
                output.returnCode = FEEDREG_ERR_TIME_POLICY;
                return;
            }
        }
        else
        {
            output.returnCode = FEEDREG_ERR_TIME_POLICY;
            return;
        }

        if (input.definition.valueType == OI::HttpRead::ValueType::decimal)
        {
            if (input.definition.decimals > OI::HttpRead::MAX_DECIMALS)
            {
                output.returnCode = FEEDREG_ERR_VALUE_TYPE;
                return;
            }
        }
        else if (input.definition.valueType == OI::HttpRead::ValueType::text || input.definition.valueType == OI::HttpRead::ValueType::boolean)
        {
            if (input.definition.decimals != 0)
            {
                output.returnCode = FEEDREG_ERR_VALUE_TYPE;
                return;
            }
        }
        else
        {
            output.returnCode = FEEDREG_ERR_VALUE_TYPE;
            return;
        }
        output.returnCode = FEEDREG_OK;
    }

    // One request parameter: 32 bytes, printable, zero padded, checked against its kind.
    struct ValidateParam_input { uint8 kind; Array<uint8, 32> value; };
    struct ValidateParam_output { bit ok; };
    struct ValidateParam_locals
    {
        uint32 i;
        uint32 len;
        uint32 intDigits;
        uint32 fracDigits;
        uint64 intValue;
        uint64 year;
        uint64 month;
        uint64 day;
        DateAndTime date;
        uint8 c;
        bit ended;
        bit fracNonZero;
    };
    PRIVATE_FUNCTION_WITH_LOCALS(ValidateParam)
    {
        output.ok = false;
        locals.len = 0;
        locals.ended = false;
        for (locals.i = 0; locals.i < 32; locals.i++)
        {
            locals.c = input.value.get(locals.i);
            if (locals.ended)
            {
                if (locals.c != 0)
                {
                    return;
                }
            }
            else if (locals.c == 0)
            {
                locals.ended = true;
            }
            else
            {
                locals.len++;
            }
        }
        if (locals.len == 0)
        {
            return;
        }

        if (input.kind == FEEDREG_PARAM_ISO_DATE)
        {
            if (locals.len != 10 || input.value.get(4) != FEEDREG_CH_DASH || input.value.get(7) != FEEDREG_CH_DASH)
            {
                return;
            }
            locals.year = 0;
            locals.month = 0;
            locals.day = 0;
            for (locals.i = 0; locals.i < 10; locals.i++)
            {
                if (locals.i == 4 || locals.i == 7)
                {
                    continue;
                }
                locals.c = input.value.get(locals.i);
                if (locals.c < FEEDREG_CH_0 || locals.c > FEEDREG_CH_9)
                {
                    return;
                }
                if (locals.i < 4)
                {
                    locals.year = locals.year * 10 + (locals.c - FEEDREG_CH_0);
                }
                else if (locals.i < 7)
                {
                    locals.month = locals.month * 10 + (locals.c - FEEDREG_CH_0);
                }
                else
                {
                    locals.day = locals.day * 10 + (locals.c - FEEDREG_CH_0);
                }
            }
            output.ok = locals.date.setIfValid(locals.year, locals.month, locals.day, 0, 0, 0);
            return;
        }

        if (input.kind == FEEDREG_PARAM_CURRENCY)
        {
            if (locals.len != 3)
            {
                return;
            }
            for (locals.i = 0; locals.i < 3; locals.i++)
            {
                locals.c = input.value.get(locals.i);
                if (locals.c < FEEDREG_CH_UPPER_A || locals.c > FEEDREG_CH_UPPER_Z)
                {
                    return;
                }
            }
            output.ok = true;
            return;
        }

        if (input.kind == FEEDREG_PARAM_SLUG)
        {
            for (locals.i = 0; locals.i < locals.len; locals.i++)
            {
                locals.c = input.value.get(locals.i);
                if (!((locals.c >= FEEDREG_CH_LOWER_A && locals.c <= FEEDREG_CH_LOWER_Z) || (locals.c >= FEEDREG_CH_0 && locals.c <= FEEDREG_CH_9)
                    || locals.c == FEEDREG_CH_DASH))
                {
                    return;
                }
            }
            output.ok = true;
            return;
        }

        if (input.kind != FEEDREG_PARAM_INTEGER && input.kind != FEEDREG_PARAM_DECIMAL && input.kind != FEEDREG_PARAM_COORDINATE)
        {
            return;
        }

        // numbers: -?digits(.digits)?
        locals.i = 0;
        if (input.value.get(0) == FEEDREG_CH_DASH)
        {
            locals.i = 1;
        }
        locals.intDigits = 0;
        locals.intValue = 0;
        while (locals.i < locals.len && input.value.get(locals.i) >= FEEDREG_CH_0 && input.value.get(locals.i) <= FEEDREG_CH_9)
        {
            locals.intValue = locals.intValue * 10 + (input.value.get(locals.i) - FEEDREG_CH_0);
            locals.intDigits++;
            locals.i++;
        }
        locals.fracDigits = 0;
        locals.fracNonZero = false;
        if (locals.i < locals.len && input.value.get(locals.i) == FEEDREG_CH_DOT && input.kind != FEEDREG_PARAM_INTEGER)
        {
            locals.i++;
            while (locals.i < locals.len && input.value.get(locals.i) >= FEEDREG_CH_0 && input.value.get(locals.i) <= FEEDREG_CH_9)
            {
                if (input.value.get(locals.i) != FEEDREG_CH_0)
                {
                    locals.fracNonZero = true;
                }
                locals.fracDigits++;
                locals.i++;
            }
            if (locals.fracDigits == 0)
            {
                return;
            }
        }
        if (locals.i != locals.len || locals.intDigits == 0 || locals.intDigits > 18 || locals.fracDigits > 18)
        {
            return;
        }
        if (input.kind == FEEDREG_PARAM_COORDINATE)
        {
            if (locals.intDigits > 3 || locals.fracDigits > 4 || locals.intValue > 180 || (locals.intValue == 180 && locals.fracNonZero))
            {
                return;
            }
        }
        output.ok = true;
    }

    // ------------------------------------------------------------------ procedures

public:

    struct ProposeFeed_input
    {
        FeedDefinition definition;
        id restrictedTo;
        sint64 feePerUse;
    };
    struct ProposeFeed_output
    {
        uint32 returnCode;
        uint64 feedId;
        id definitionHash;
    };
    struct ProposeFeed_locals
    {
        ValidateDefinition_input v;
        ValidateDefinition_output vOut;
        Feed feed;
        uint64 existing;
    };
    // Invocation reward must be at least the current deposit; the excess is refunded.
    PUBLIC_PROCEDURE_WITH_LOCALS(ProposeFeed)
    {
        output.returnCode = FEEDREG_OK;
        output.feedId = 0;
        output.definitionHash = NULL_ID;

        if (qpi.invocationReward() < state.get().proposeDeposit)
        {
            output.returnCode = FEEDREG_ERR_DEPOSIT;
        }
        else if (state.get().feedCount >= FEEDREG_MAX_FEEDS)
        {
            output.returnCode = FEEDREG_ERR_FULL;
        }
        else if (input.feePerUse < 0 || input.feePerUse > FEEDREG_MAX_FEE_PER_USE)
        {
            output.returnCode = FEEDREG_ERR_FEE;
        }
        else
        {
            locals.v.definition = input.definition;
            CALL(ValidateDefinition, locals.v, locals.vOut);
            output.returnCode = locals.vOut.returnCode;
        }
        if (output.returnCode == FEEDREG_OK)
        {
            output.definitionHash = qpi.K12(input.definition);
            if (state.get().liveHashes.get(output.definitionHash, locals.existing))
            {
                output.returnCode = FEEDREG_ERR_DUPLICATE;
            }
        }
        if (output.returnCode != FEEDREG_OK)
        {
            output.definitionHash = NULL_ID;
            if (qpi.invocationReward() > 0)
            {
                qpi.transfer(qpi.invocator(), qpi.invocationReward());
            }
            return;
        }

        setMemory(locals.feed, 0);
        locals.feed.definition = input.definition;
        locals.feed.definitionHash = output.definitionHash;
        locals.feed.proposer = qpi.invocator();
        locals.feed.restrictedTo = input.restrictedTo;
        locals.feed.feePerUse = input.feePerUse;
        locals.feed.deposit = state.get().proposeDeposit;
        locals.feed.status = FEEDREG_STATUS_PENDING;
        locals.feed.proposedEpoch = qpi.epoch();

        output.feedId = state.get().feedCount + 1;
        state.mut().feeds.set(state.get().feedCount, locals.feed);
        state.mut().feedCount = output.feedId;
        state.mut().liveHashes.set(output.definitionHash, output.feedId);
        state.mut().heldDeposits += locals.feed.deposit;

        if (qpi.invocationReward() > locals.feed.deposit)
        {
            qpi.transfer(qpi.invocator(), qpi.invocationReward() - locals.feed.deposit);
        }
    }

    struct WithdrawFeed_input { uint64 feedId; };
    struct WithdrawFeed_output { uint32 returnCode; sint64 refunded; };
    struct WithdrawFeed_locals { Feed feed; sint64 half; };
    // Proposer takes back a PENDING feed. Half the deposit is refunded, half burned,
    // so proposing and withdrawing is not free spam.
    PUBLIC_PROCEDURE_WITH_LOCALS(WithdrawFeed)
    {
        output.refunded = 0;
        if (qpi.invocationReward() > 0)
        {
            qpi.transfer(qpi.invocator(), qpi.invocationReward());
        }
        if (input.feedId == 0 || input.feedId > state.get().feedCount)
        {
            output.returnCode = FEEDREG_ERR_NOT_FOUND;
            return;
        }
        locals.feed = state.get().feeds.get(input.feedId - 1);
        if (locals.feed.proposer != qpi.invocator())
        {
            output.returnCode = FEEDREG_ERR_NOT_PROPOSER;
            return;
        }
        if (locals.feed.status != FEEDREG_STATUS_PENDING)
        {
            output.returnCode = FEEDREG_ERR_WRONG_STATUS;
            return;
        }
        locals.half = div<sint64>(locals.feed.deposit, 2LL);
        output.refunded = locals.feed.deposit - locals.half;
        state.mut().heldDeposits -= locals.feed.deposit;
        if (locals.half > 0)
        {
            qpi.burn(locals.half);
        }
        if (output.refunded > 0)
        {
            qpi.transfer(locals.feed.proposer, output.refunded);
        }
        locals.feed.deposit = 0;
        locals.feed.status = FEEDREG_STATUS_WITHDRAWN;
        locals.feed.decidedEpoch = qpi.epoch();
        state.mut().feeds.set(input.feedId - 1, locals.feed);
        state.mut().liveHashes.removeByKey(locals.feed.definitionHash);
        output.returnCode = FEEDREG_OK;
    }

    // ------------------------------------------------------------------ reading a feed

    // Oracle callback: never callable by users (registered as a notification only).
    typedef OracleNotificationInput<OI::HttpRead> NotifyHttpRead_input;
    typedef NoData NotifyHttpRead_output;
    struct NotifyHttpRead_locals
    {
        uint64 requestId;
        uint64 slot;
        Request req;
        Feed feed;
        sint64 proposerCut;
    };
    PRIVATE_PROCEDURE_WITH_LOCALS(NotifyHttpRead)
    {
        if (!state.get().queryToRequest.get(input.queryId, locals.requestId))
        {
            return;
        }
        state.mut().queryToRequest.removeByKey(input.queryId);
        locals.slot = mod<uint64>(locals.requestId, FEEDREG_MAX_REQUESTS);
        locals.req = state.get().requests.get(locals.slot);
        if (locals.req.requestId != locals.requestId || locals.req.status != FEEDREG_REQUEST_PENDING)
        {
            return;
        }
        state.mut().heldFees -= locals.req.feeHeld;
        if (input.status == ORACLE_QUERY_STATUS_SUCCESS)
        {
            locals.req.status = FEEDREG_REQUEST_ANSWERED;
            locals.req.reply = input.reply;
            locals.feed = state.get().feeds.get(locals.req.feedId - 1);
            locals.feed.requestCount++;
            state.mut().feeds.set(locals.req.feedId - 1, locals.feed);
            locals.proposerCut = div<sint64>(locals.req.feeHeld * state.get().proposerShareBps, 10000LL);
            if (locals.proposerCut > 0)
            {
                qpi.transfer(locals.feed.proposer, locals.proposerCut);
                state.mut().totalPaidToProposers += locals.proposerCut;
            }
            state.mut().dividendPool += locals.req.feeHeld - locals.proposerCut;
        }
        else
        {
            // timeout or no quorum: nothing was delivered, so the feed fee goes back
            locals.req.status = FEEDREG_REQUEST_NOT_ANSWERED;
            if (locals.req.feeHeld > 0)
            {
                qpi.transfer(locals.req.requester, locals.req.feeHeld);
            }
        }
        locals.req.feeHeld = 0;
        locals.req.answerTick = qpi.tick();
        state.mut().requests.set(locals.slot, locals.req);
    }

    struct RequestValue_input
    {
        uint64 feedId;
        DateAndTime timestamp;           // the moment the value is for
        Array<uint8, 128> params;        // 4 x 32 bytes, zero padded; unused parameters all zero
    };
    struct RequestValue_output
    {
        uint32 returnCode;
        uint64 requestId;                // read the answer with GetRequest once it is no longer PENDING
        sint64 charged;                  // oracle fee + feed fee
    };
    struct RequestValue_locals
    {
        Feed feed;
        Request req;
        OI::HttpRead::OracleQuery query;
        DateAndTime latest;
        ValidateParamKinds_input kinds;
        ValidateParamKinds_output kindsOut;
        ValidateParam_input param;
        ValidateParam_output paramOut;
        uint32 i;
        uint32 j;
        uint64 slot;
        sint64 oracleFee;
        sint64 cost;
        sint64 queryId;
    };
    // Invocation reward must cover the oracle fee plus the feed's feePerUse (see GetFeed's requestCost).
    PUBLIC_PROCEDURE_WITH_LOCALS(RequestValue)
    {
        output.returnCode = FEEDREG_OK;
        output.requestId = 0;
        output.charged = 0;

        if (input.feedId == 0 || input.feedId > state.get().feedCount)
        {
            output.returnCode = FEEDREG_ERR_NOT_FOUND;
        }
        else
        {
            locals.feed = state.get().feeds.get(input.feedId - 1);
            if (locals.feed.status != FEEDREG_STATUS_APPROVED)
            {
                output.returnCode = FEEDREG_ERR_WRONG_STATUS;
            }
            else if (locals.feed.restrictedTo != NULL_ID && locals.feed.restrictedTo != qpi.invocator())
            {
                output.returnCode = FEEDREG_ERR_RESTRICTED;
            }
            else if (!input.timestamp.isValid())
            {
                output.returnCode = FEEDREG_ERR_TIMESTAMP;
            }
            else
            {
                locals.latest = qpi.now();
                locals.latest.add(0, 0, 0, 0, 0, -(sint64)locals.feed.definition.minAgeSeconds);
                if (input.timestamp > locals.latest)
                {
                    output.returnCode = FEEDREG_ERR_TOO_RECENT;
                }
            }
        }

        // parameters: declared ones must pass their kind, the others must be all zero
        if (output.returnCode == FEEDREG_OK)
        {
            locals.kinds.kinds = locals.feed.definition.paramKinds;
            CALL(ValidateParamKinds, locals.kinds, locals.kindsOut);
            for (locals.j = 0; locals.j < 4 && output.returnCode == FEEDREG_OK; locals.j++)
            {
                for (locals.i = 0; locals.i < 32; locals.i++)
                {
                    locals.param.value.set(locals.i, input.params.get(locals.j * 32 + locals.i));
                }
                if (locals.j < locals.kindsOut.count)
                {
                    locals.param.kind = locals.feed.definition.paramKinds.get(locals.j);
                    CALL(ValidateParam, locals.param, locals.paramOut);
                    if (!locals.paramOut.ok)
                    {
                        output.returnCode = FEEDREG_ERR_PARAMS;
                    }
                }
                else
                {
                    for (locals.i = 0; locals.i < 32; locals.i++)
                    {
                        if (locals.param.value.get(locals.i) != 0)
                        {
                            output.returnCode = FEEDREG_ERR_PARAMS;
                        }
                    }
                }
            }
        }

        if (output.returnCode == FEEDREG_OK)
        {
            locals.slot = mod<uint64>(state.get().nextRequestId, FEEDREG_MAX_REQUESTS);
            if (state.get().requests.get(locals.slot).status == FEEDREG_REQUEST_PENDING)
            {
                output.returnCode = FEEDREG_ERR_BUSY;
            }
        }

        if (output.returnCode == FEEDREG_OK)
        {
            // HttpRead query for the feedreg source: the request id goes in endpoint, the field is unused
            setMemory(locals.query, 0);
            locals.query.source = OI::HttpRead::getFeedRegistrySourceId();
            locals.query.endpoint = state.get().nextRequestId;
            locals.query.timestamp = input.timestamp;
            locals.query.params = input.params;
            locals.query.valueType = locals.feed.definition.valueType;
            locals.query.decimals = locals.feed.definition.decimals;
            locals.oracleFee = OI::HttpRead::getQueryFee(locals.query);
            locals.cost = locals.oracleFee + locals.feed.feePerUse;
            if (qpi.invocationReward() < locals.cost)
            {
                output.returnCode = FEEDREG_ERR_PAYMENT;
            }
        }

        if (output.returnCode == FEEDREG_OK)
        {
            locals.queryId = QUERY_ORACLE(OI::HttpRead, locals.query, NotifyHttpRead, FEEDREG_ORACLE_TIMEOUT_MS);
            if (locals.queryId < 0)
            {
                output.returnCode = FEEDREG_ERR_ORACLE_REFUSED;
            }
        }

        if (output.returnCode != FEEDREG_OK)
        {
            if (qpi.invocationReward() > 0)
            {
                qpi.transfer(qpi.invocator(), qpi.invocationReward());
            }
            return;
        }

        setMemory(locals.req, 0);
        locals.req.requestId = state.get().nextRequestId;
        locals.req.feedId = input.feedId;
        locals.req.requester = qpi.invocator();
        locals.req.definitionHash = locals.feed.definitionHash;
        locals.req.timestamp = input.timestamp;
        locals.req.params = input.params;
        locals.req.oracleQueryId = locals.queryId;
        locals.req.feeHeld = locals.feed.feePerUse;
        locals.req.status = FEEDREG_REQUEST_PENDING;
        locals.req.requestTick = qpi.tick();
        state.mut().requests.set(locals.slot, locals.req);
        state.mut().queryToRequest.set(locals.queryId, locals.req.requestId);
        state.mut().heldFees += locals.feed.feePerUse;
        state.mut().nextRequestId++;

        output.requestId = locals.req.requestId;
        output.charged = locals.cost;
        if (qpi.invocationReward() > locals.cost)
        {
            qpi.transfer(qpi.invocator(), qpi.invocationReward() - locals.cost);
        }
    }

    // ------------------------------------------------------------------ functions

    struct GetFeed_input { uint64 feedId; };
    struct GetFeed_output { uint32 returnCode; sint64 requestCost; Feed feed; };
    struct GetFeed_locals { OI::HttpRead::OracleQuery query; };
    PUBLIC_FUNCTION_WITH_LOCALS(GetFeed)
    {
        if (input.feedId == 0 || input.feedId > state.get().feedCount)
        {
            output.returnCode = FEEDREG_ERR_NOT_FOUND;
            return;
        }
        output.returnCode = FEEDREG_OK;
        output.feed = state.get().feeds.get(input.feedId - 1);
        locals.query.source = OI::HttpRead::getFeedRegistrySourceId();
        output.requestCost = OI::HttpRead::getQueryFee(locals.query) + output.feed.feePerUse;
    }

    // What consumer contracts read for their answer, and what an oracle machine
    // reads before serving a feedreg query.
    struct GetRequest_input { uint64 requestId; };
    struct GetRequest_output { uint32 returnCode; Request request; };
    struct GetRequest_locals { Request req; };
    PUBLIC_FUNCTION_WITH_LOCALS(GetRequest)
    {
        locals.req = state.get().requests.get(mod<uint64>(input.requestId, FEEDREG_MAX_REQUESTS));
        if (input.requestId == 0 || locals.req.requestId != input.requestId)
        {
            output.returnCode = FEEDREG_ERR_NOT_FOUND;   // never issued, or overwritten by a newer request
            return;
        }
        output.returnCode = FEEDREG_OK;
        output.request = locals.req;
    }

    struct FeedSummary
    {
        uint64 feedId;
        id definitionHash;
        sint64 feePerUse;
        uint64 requestCount;
        uint32 status;
        uint8 restricted;
        uint8 timePolicy;
        uint8 valueType;
        uint8 decimals;
    };
    struct ListFeeds_input { uint64 startFeedId; uint32 status; };   // status 0: any
    struct ListFeeds_output
    {
        uint32 count;
        uint64 nextFeedId;                       // 0: no more
        Array<FeedSummary, FEEDREG_LIST_PAGE> feeds;
    };
    struct ListFeeds_locals { uint64 i; Feed feed; FeedSummary s; };
    PUBLIC_FUNCTION_WITH_LOCALS(ListFeeds)
    {
        output.count = 0;
        output.nextFeedId = 0;
        locals.i = input.startFeedId;
        if (locals.i == 0)
        {
            locals.i = 1;
        }
        for (; locals.i <= state.get().feedCount; locals.i++)
        {
            locals.feed = state.get().feeds.get(locals.i - 1);
            if (input.status != 0 && locals.feed.status != input.status)
            {
                continue;
            }
            if (output.count == FEEDREG_LIST_PAGE)
            {
                output.nextFeedId = locals.i;
                return;
            }
            locals.s.feedId = locals.i;
            locals.s.definitionHash = locals.feed.definitionHash;
            locals.s.feePerUse = locals.feed.feePerUse;
            locals.s.requestCount = locals.feed.requestCount;
            locals.s.status = locals.feed.status;
            locals.s.restricted = (locals.feed.restrictedTo != NULL_ID) ? 1 : 0;
            locals.s.timePolicy = locals.feed.definition.timePolicy;
            locals.s.valueType = locals.feed.definition.valueType;
            locals.s.decimals = locals.feed.definition.decimals;
            output.feeds.set(output.count, locals.s);
            output.count++;
        }
    }

    struct FindByHash_input { id definitionHash; };
    struct FindByHash_output { uint64 feedId; };   // 0: not PENDING or APPROVED
    PUBLIC_FUNCTION(FindByHash)
    {
        if (!state.get().liveHashes.get(input.definitionHash, output.feedId))
        {
            output.feedId = 0;
        }
    }

    // Lets a proposer check a definition and see its hash without paying.
    struct CheckDefinition_input { FeedDefinition definition; };
    struct CheckDefinition_output { uint32 returnCode; id definitionHash; };
    struct CheckDefinition_locals { ValidateDefinition_input v; ValidateDefinition_output vOut; };
    PUBLIC_FUNCTION_WITH_LOCALS(CheckDefinition)
    {
        locals.v.definition = input.definition;
        CALL(ValidateDefinition, locals.v, locals.vOut);
        output.returnCode = locals.vOut.returnCode;
        output.definitionHash = NULL_ID;
        if (output.returnCode == FEEDREG_OK)
        {
            output.definitionHash = qpi.K12(input.definition);
        }
    }

    typedef NoData GetInfo_input;
    struct GetInfo_output
    {
        uint64 feedCount;
        uint64 nextRequestId;
        sint64 proposeDeposit;
        sint64 proposerShareBps;
        sint64 proposalFee;
        sint64 heldDeposits;
        sint64 heldFees;
        sint64 dividendPool;
        sint64 totalPaidToShareholders;
        sint64 totalPaidToProposers;
    };
    PUBLIC_FUNCTION(GetInfo)
    {
        output.feedCount = state.get().feedCount;
        output.nextRequestId = state.get().nextRequestId;
        output.proposeDeposit = state.get().proposeDeposit;
        output.proposerShareBps = state.get().proposerShareBps;
        output.proposalFee = state.get().proposalFee;
        output.heldDeposits = state.get().heldDeposits;
        output.heldFees = state.get().heldFees;
        output.dividendPool = state.get().dividendPool;
        output.totalPaidToShareholders = state.get().totalPaidToShareholders;
        output.totalPaidToProposers = state.get().totalPaidToProposers;
    }

    // ------------------------------------------------------------------ shareholder governance

    typedef ProposalDataYesNo SetShareholderProposal_input;   // same type as ProposalDataT; the verifier knows this name
    typedef uint16 SetShareholderProposal_output;
    struct SetShareholderProposal_locals { Feed feed; bit valid; };
    PUBLIC_PROCEDURE_WITH_LOCALS(SetShareholderProposal)
    {
        locals.valid = qpi.invocationReward() >= state.get().proposalFee;
        if (locals.valid && input.epoch != 0)
        {
            locals.valid = input.type == ProposalTypes::VariableYesNo
                && input.data.variableOptions.variable < FEEDREG_VARIABLE_COUNT
                && input.data.variableOptions.value >= 0;
            if (locals.valid)
            {
                if (input.data.variableOptions.variable == FEEDREG_VAR_APPROVE_FEED || input.data.variableOptions.variable == FEEDREG_VAR_RETIRE_FEED)
                {
                    locals.valid = input.data.variableOptions.value > 0 && (uint64)input.data.variableOptions.value <= state.get().feedCount;
                    if (locals.valid)
                    {
                        locals.feed = state.get().feeds.get(input.data.variableOptions.value - 1);
                        locals.valid = (input.data.variableOptions.variable == FEEDREG_VAR_APPROVE_FEED)
                            ? (locals.feed.status == FEEDREG_STATUS_PENDING)
                            : (locals.feed.status == FEEDREG_STATUS_APPROVED);
                    }
                }
                else if (input.data.variableOptions.variable == FEEDREG_VAR_DEPOSIT)
                {
                    locals.valid = input.data.variableOptions.value >= FEEDREG_MIN_DEPOSIT && input.data.variableOptions.value <= FEEDREG_MAX_DEPOSIT;
                }
                else if (input.data.variableOptions.variable == FEEDREG_VAR_PROPOSER_SHARE_BPS)
                {
                    locals.valid = input.data.variableOptions.value <= FEEDREG_MAX_PROPOSER_SHARE_BPS;
                }
                else
                {
                    locals.valid = input.data.variableOptions.value <= FEEDREG_MAX_PROPOSAL_FEE;
                }
            }
        }
        if (!locals.valid)
        {
            if (qpi.invocationReward() > 0)
            {
                qpi.transfer(qpi.invocator(), qpi.invocationReward());
            }
            output = INVALID_PROPOSAL_INDEX;
            return;
        }
        output = qpi(state.mut().proposals).setProposal(qpi.invocator(), input);
        if (output == INVALID_PROPOSAL_INDEX)
        {
            if (qpi.invocationReward() > 0)
            {
                qpi.transfer(qpi.invocator(), qpi.invocationReward());
            }
            return;
        }
        if (state.get().proposalFee > 0)
        {
            qpi.burn(state.get().proposalFee);
        }
        if (qpi.invocationReward() > state.get().proposalFee)
        {
            qpi.transfer(qpi.invocator(), qpi.invocationReward() - state.get().proposalFee);
        }
    }

    IMPLEMENT_GetShareholderProposal()
    IMPLEMENT_GetShareholderProposalIndices()
    IMPLEMENT_GetShareholderProposalFees(state.get().proposalFee)
    IMPLEMENT_SetShareholderVotes()
    IMPLEMENT_GetShareholderVotes()
    IMPLEMENT_GetShareholderVotingResults()
    IMPLEMENT_SET_SHAREHOLDER_PROPOSAL()
    IMPLEMENT_SET_SHAREHOLDER_VOTES()

    // ------------------------------------------------------------------ system procedures

    INITIALIZE()
    {
        state.mut().proposeDeposit = FEEDREG_DEFAULT_DEPOSIT;
        state.mut().proposerShareBps = FEEDREG_DEFAULT_PROPOSER_SHARE_BPS;
        state.mut().proposalFee = FEEDREG_DEFAULT_PROPOSAL_FEE;
        state.mut().nextRequestId = 1;
    }

    struct END_EPOCH_locals
    {
        sint32 proposalIndex;
        ProposalDataT proposal;
        ProposalSummarizedVotingDataV1 results;
        uint64 variable;
        sint64 value;
        uint64 i;
        Feed feed;
        sint64 perShare;
    };
    END_EPOCH_WITH_LOCALS()
    {
        // 1. accepted proposals of this epoch
        locals.proposalIndex = -1;
        while ((locals.proposalIndex = qpi(state.get().proposals).nextProposalIndex(locals.proposalIndex, qpi.epoch())) >= 0)
        {
            if (!qpi(state.get().proposals).getProposal(locals.proposalIndex, locals.proposal))
            {
                continue;
            }
            if (locals.proposal.type != ProposalTypes::VariableYesNo)
            {
                continue;
            }
            if (!qpi(state.get().proposals).getVotingSummary(locals.proposalIndex, locals.results))
            {
                continue;
            }
            if (locals.results.getAcceptedOption() != 1)
            {
                continue;
            }
            locals.variable = locals.proposal.data.variableOptions.variable;
            locals.value = locals.proposal.data.variableOptions.value;
            if (locals.variable == FEEDREG_VAR_APPROVE_FEED || locals.variable == FEEDREG_VAR_RETIRE_FEED)
            {
                if (locals.value <= 0 || (uint64)locals.value > state.get().feedCount)
                {
                    continue;
                }
                locals.feed = state.get().feeds.get(locals.value - 1);
                if (locals.variable == FEEDREG_VAR_APPROVE_FEED && locals.feed.status == FEEDREG_STATUS_PENDING)
                {
                    state.mut().heldDeposits -= locals.feed.deposit;
                    if (locals.feed.deposit > 0)
                    {
                        qpi.transfer(locals.feed.proposer, locals.feed.deposit);
                    }
                    locals.feed.deposit = 0;
                    locals.feed.status = FEEDREG_STATUS_APPROVED;
                    locals.feed.decidedEpoch = qpi.epoch();
                    state.mut().feeds.set(locals.value - 1, locals.feed);
                }
                else if (locals.variable == FEEDREG_VAR_RETIRE_FEED && locals.feed.status == FEEDREG_STATUS_APPROVED)
                {
                    locals.feed.status = FEEDREG_STATUS_RETIRED;
                    locals.feed.decidedEpoch = qpi.epoch();
                    state.mut().feeds.set(locals.value - 1, locals.feed);
                    state.mut().liveHashes.removeByKey(locals.feed.definitionHash);
                }
            }
            else if (locals.variable == FEEDREG_VAR_DEPOSIT)
            {
                state.mut().proposeDeposit = locals.value;
            }
            else if (locals.variable == FEEDREG_VAR_PROPOSER_SHARE_BPS)
            {
                state.mut().proposerShareBps = locals.value;
            }
            else if (locals.variable == FEEDREG_VAR_PROPOSAL_FEE)
            {
                state.mut().proposalFee = locals.value;
            }
        }

        // 2. PENDING feeds that had their last voting epoch now: expired, deposit burned
        for (locals.i = 0; locals.i < state.get().feedCount; locals.i++)
        {
            locals.feed = state.get().feeds.get(locals.i);
            if (locals.feed.status != FEEDREG_STATUS_PENDING || qpi.epoch() + 1 < locals.feed.proposedEpoch + FEEDREG_PENDING_EPOCHS)
            {
                continue;
            }
            state.mut().heldDeposits -= locals.feed.deposit;
            if (locals.feed.deposit > 0)
            {
                qpi.burn(locals.feed.deposit);
            }
            locals.feed.deposit = 0;
            locals.feed.status = FEEDREG_STATUS_EXPIRED;
            locals.feed.decidedEpoch = qpi.epoch();
            state.mut().feeds.set(locals.i, locals.feed);
            state.mut().liveHashes.removeByKey(locals.feed.definitionHash);
        }

        // 3. feed fees earned this epoch go to shareholders
        locals.perShare = div<sint64>(state.get().dividendPool, (sint64)NUMBER_OF_COMPUTORS);
        if (locals.perShare > 0 && qpi.distributeDividends(locals.perShare))
        {
            state.mut().dividendPool -= locals.perShare * NUMBER_OF_COMPUTORS;
            state.mut().totalPaidToShareholders += locals.perShare * NUMBER_OF_COMPUTORS;
        }

        state.mut().queryToRequest.cleanupIfNeeded();
        state.mut().liveHashes.cleanupIfNeeded();
    }

    REGISTER_USER_FUNCTIONS_AND_PROCEDURES()
    {
        REGISTER_USER_FUNCTION(GetFeed, 1);
        REGISTER_USER_FUNCTION(ListFeeds, 2);
        REGISTER_USER_FUNCTION(GetRequest, 3);
        REGISTER_USER_FUNCTION(FindByHash, 4);
        REGISTER_USER_FUNCTION(CheckDefinition, 5);
        REGISTER_USER_FUNCTION(GetInfo, 6);

        REGISTER_USER_PROCEDURE(ProposeFeed, 1);
        REGISTER_USER_PROCEDURE(WithdrawFeed, 2);
        REGISTER_USER_PROCEDURE(RequestValue, 3);

        // oracle callback: registered as a notification only, never callable by users
        REGISTER_USER_PROCEDURE_NOTIFICATION(NotifyHttpRead);

        REGISTER_SHAREHOLDER_PROPOSAL_VOTING();
    }
};
