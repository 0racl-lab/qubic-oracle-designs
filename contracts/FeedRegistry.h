using namespace QPI;

// ============================================================================
// FeedRegistry (contract FEEDREG) - a public list of web data feeds that
// Qubic oracle machines may fetch, governed by the registry's shareholders.
//
// DESIGN SKETCH. Not proposed to qubic/core. It exists so core and oracle
// machine developers can read concrete rules instead of prose. See
// designs/http-read.md, "Feeds governed by a registry contract".
//
// The split:
//   - This contract stores feed definitions: which https URL template, which
//     parameter kinds, which JSON value, how old the data must be. Anyone can
//     propose one with a deposit. Shareholders vote it in. Only approved feeds
//     are served.
//   - A separate oracle interface (FeedRead, drafted in the design doc) carries
//     a feedId, the feed's definition hash and the parameters. Every oracle
//     machine reads the entry from this contract's public state, checks that
//     the feed is approved and that the hash matches, validates the parameters
//     and only then fetches. Machines that cannot verify abstain.
//
// Why the definition hash: contract state is public, but it can change (a feed
// can be retired). A query pinned to the hash of the definition it was written
// against can never be served from a different definition.
//
// Lifecycle of a feed:
//   ProposeFeed (deposit) -> PENDING
//   PENDING -- shareholder vote "approve feedId" accepted at END_EPOCH --> APPROVED, deposit refunded
//   PENDING -- no accepted vote within FEEDREG_PENDING_EPOCHS epochs --> EXPIRED, deposit burned
//   PENDING -- WithdrawFeed by the proposer --> WITHDRAWN, half the deposit refunded, half burned
//   APPROVED -- shareholder vote "retire feedId" accepted --> RETIRED
// Definitions never change after proposal. Feed ids are never reused.
//
// Access and fees: a feed may charge feePerEpoch and may be restricted to one
// identity. BuyAccess pays for whole epochs. The proposer receives
// proposerShareBps of each payment, the rest is paid to shareholders as
// dividends at END_EPOCH. An oracle machine serving a feed with a fee or a
// restriction checks GetAccess(feedId, user) for the user named in the query.
// OPEN POINT: the oracle machine is not told which contract sent a query, so
// the user in the query is self-declared. Real enforcement needs core to pass
// the querying contract's index to the machine (see the design doc).
//
// Shareholder proposals (yes/no, applied at END_EPOCH):
//   variable 0  approve feed <value = feedId>     (feed must be PENDING)
//   variable 1  retire feed <value = feedId>      (feed must be APPROVED)
//   variable 2  propose deposit in QU             (1000 .. 10^12)
//   variable 3  proposer share in basis points    (0 .. 5000)
//   variable 4  shareholder proposal fee in QU    (0 .. 10^10)
// ============================================================================

constexpr uint64 FEEDREG_CONTRACT_ASSET_NAME = 20060942981481798;   // "FEEDREG"
constexpr uint64 FEEDREG_MAX_FEEDS = 1024;
constexpr uint64 FEEDREG_MAX_ACCESS = 4096;
constexpr uint32 FEEDREG_PROPOSAL_SLOTS = 16;
constexpr uint16 FEEDREG_PENDING_EPOCHS = 4;
constexpr uint16 FEEDREG_MAX_ACCESS_EPOCHS = 52;
constexpr uint32 FEEDREG_LIST_PAGE = 16;

constexpr sint64 FEEDREG_DEFAULT_DEPOSIT = 10000000;
constexpr sint64 FEEDREG_DEFAULT_PROPOSER_SHARE_BPS = 2000;
constexpr sint64 FEEDREG_DEFAULT_PROPOSAL_FEE = 1000000;
constexpr sint64 FEEDREG_MIN_DEPOSIT = 1000;
constexpr sint64 FEEDREG_MAX_DEPOSIT = 1000000000000;
constexpr sint64 FEEDREG_MAX_PROPOSER_SHARE_BPS = 5000;
constexpr sint64 FEEDREG_MAX_PROPOSAL_FEE = 10000000000;
constexpr sint64 FEEDREG_MAX_FEE_PER_EPOCH = 1000000000000;

// minimum age of data for a HISTORICAL feed: 1 minute .. 31 days
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

// parameter kinds; the oracle machine validates each parameter against its kind
constexpr uint8 FEEDREG_PARAM_NONE = 0;
constexpr uint8 FEEDREG_PARAM_ISO_DATE = 1;        // YYYY-MM-DD
constexpr uint8 FEEDREG_PARAM_CURRENCY = 2;        // 3 upper-case letters
constexpr uint8 FEEDREG_PARAM_INTEGER = 3;         // -?[0-9]{1,18}
constexpr uint8 FEEDREG_PARAM_DECIMAL = 4;         // -?[0-9]{1,18}(\.[0-9]{1,18})?
constexpr uint8 FEEDREG_PARAM_SLUG = 5;            // [a-z0-9-]{1,32}
constexpr uint8 FEEDREG_PARAM_COORDINATE = 6;      // decimal with at most 4 fraction digits, |v| <= 180
constexpr uint8 FEEDREG_PARAM_KIND_COUNT = 7;

// time policy
constexpr uint8 FEEDREG_TIME_HISTORICAL = 1;       // data for a timestamp at least minAgeSeconds old
constexpr uint8 FEEDREG_TIME_FINAL = 2;            // the source marks the record final at finalPath

// value type of the extracted JSON value
constexpr uint8 FEEDREG_VALUE_INTEGER = 1;
constexpr uint8 FEEDREG_VALUE_DECIMAL = 2;         // scaled to an integer by 10^decimals
constexpr uint8 FEEDREG_VALUE_TEXT = 3;
constexpr uint8 FEEDREG_VALUE_BOOL = 4;
constexpr uint8 FEEDREG_MAX_DECIMALS = 18;

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
constexpr uint32 FEEDREG_ERR_EPOCHS = 19;
constexpr uint32 FEEDREG_ERR_PAYMENT = 20;
constexpr uint32 FEEDREG_ERR_ACCESS_FULL = 21;

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
        uint32 minAgeSeconds;            // HISTORICAL: data must be at least this old
        uint8 timePolicy;                // FEEDREG_TIME_*
        uint8 valueType;                 // FEEDREG_VALUE_*
        uint8 decimals;                  // DECIMAL: 1..18, else 0
        uint8 reserved;                  // must be zero
    };

    struct Feed
    {
        FeedDefinition definition;
        id definitionHash;
        id proposer;
        id restrictedTo;                 // NULL_ID: anyone may buy access
        sint64 feePerEpoch;
        sint64 deposit;                  // held while PENDING
        uint32 status;
        uint16 proposedEpoch;
        uint16 decidedEpoch;
    };

    struct AccessKey
    {
        id user;
        uint64 feedId;
    };

    struct StateData
    {
        ProposalVotingT proposals;
        Array<Feed, FEEDREG_MAX_FEEDS> feeds;              // feedId = index + 1
        HashMap<id, uint64, FEEDREG_MAX_FEEDS> liveHashes; // definitionHash -> feedId while PENDING or APPROVED
        HashMap<AccessKey, uint16, FEEDREG_MAX_ACCESS> access; // -> last epoch with access
        uint64 feedCount;
        sint64 proposeDeposit;
        sint64 proposerShareBps;
        sint64 proposalFee;
        sint64 dividendPool;
        sint64 heldDeposits;
        sint64 totalPaidToShareholders;
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

        if (input.definition.valueType == FEEDREG_VALUE_DECIMAL)
        {
            if (input.definition.decimals == 0 || input.definition.decimals > FEEDREG_MAX_DECIMALS)
            {
                output.returnCode = FEEDREG_ERR_VALUE_TYPE;
                return;
            }
        }
        else if (input.definition.valueType == FEEDREG_VALUE_INTEGER || input.definition.valueType == FEEDREG_VALUE_TEXT
            || input.definition.valueType == FEEDREG_VALUE_BOOL)
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

    // ------------------------------------------------------------------ procedures

public:

    struct ProposeFeed_input
    {
        FeedDefinition definition;
        id restrictedTo;
        sint64 feePerEpoch;
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
        else if (input.feePerEpoch < 0 || input.feePerEpoch > FEEDREG_MAX_FEE_PER_EPOCH)
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
        locals.feed.feePerEpoch = input.feePerEpoch;
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

    struct BuyAccess_input { uint64 feedId; uint16 epochs; };
    struct BuyAccess_output { uint32 returnCode; uint16 untilEpoch; sint64 paid; };
    struct BuyAccess_locals
    {
        Feed feed;
        AccessKey key;
        uint16 until;
        sint64 cost;
        sint64 proposerCut;
    };
    // Pays for access to an APPROVED feed for whole epochs, for the invocator
    // (normally the contract that will send FeedRead queries).
    PUBLIC_PROCEDURE_WITH_LOCALS(BuyAccess)
    {
        output.untilEpoch = 0;
        output.paid = 0;
        output.returnCode = FEEDREG_OK;
        if (input.feedId == 0 || input.feedId > state.get().feedCount)
        {
            output.returnCode = FEEDREG_ERR_NOT_FOUND;
        }
        else
        {
            locals.feed = state.get().feeds.get(input.feedId - 1);
            locals.cost = locals.feed.feePerEpoch * input.epochs;
            if (locals.feed.status != FEEDREG_STATUS_APPROVED)
            {
                output.returnCode = FEEDREG_ERR_WRONG_STATUS;
            }
            else if (locals.feed.restrictedTo != NULL_ID && locals.feed.restrictedTo != qpi.invocator())
            {
                output.returnCode = FEEDREG_ERR_RESTRICTED;
            }
            else if (input.epochs == 0 || input.epochs > FEEDREG_MAX_ACCESS_EPOCHS)
            {
                output.returnCode = FEEDREG_ERR_EPOCHS;
            }
            else if (qpi.invocationReward() < locals.cost)
            {
                output.returnCode = FEEDREG_ERR_PAYMENT;
            }
        }
        if (output.returnCode == FEEDREG_OK)
        {
            locals.key.user = qpi.invocator();
            locals.key.feedId = input.feedId;
            locals.until = qpi.epoch() - 1;
            if (state.get().access.get(locals.key, locals.until) && locals.until < qpi.epoch() - 1)
            {
                locals.until = qpi.epoch() - 1;
            }
            locals.until += input.epochs;
            if (locals.until - qpi.epoch() >= FEEDREG_MAX_ACCESS_EPOCHS)
            {
                output.returnCode = FEEDREG_ERR_EPOCHS;
            }
            else if (state.mut().access.set(locals.key, locals.until) == NULL_INDEX)
            {
                output.returnCode = FEEDREG_ERR_ACCESS_FULL;
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

        output.untilEpoch = locals.until;
        output.paid = locals.cost;
        locals.proposerCut = div<sint64>(locals.cost * state.get().proposerShareBps, 10000LL);
        if (locals.proposerCut > 0)
        {
            qpi.transfer(locals.feed.proposer, locals.proposerCut);
        }
        state.mut().dividendPool += locals.cost - locals.proposerCut;
        if (qpi.invocationReward() > locals.cost)
        {
            qpi.transfer(qpi.invocator(), qpi.invocationReward() - locals.cost);
        }
    }

    // ------------------------------------------------------------------ functions

    struct GetFeed_input { uint64 feedId; };
    struct GetFeed_output { uint32 returnCode; Feed feed; };
    PUBLIC_FUNCTION(GetFeed)
    {
        if (input.feedId == 0 || input.feedId > state.get().feedCount)
        {
            output.returnCode = FEEDREG_ERR_NOT_FOUND;
            return;
        }
        output.returnCode = FEEDREG_OK;
        output.feed = state.get().feeds.get(input.feedId - 1);
    }

    struct FeedSummary
    {
        uint64 feedId;
        id definitionHash;
        sint64 feePerEpoch;
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
            locals.s.feePerEpoch = locals.feed.feePerEpoch;
            locals.s.status = locals.feed.status;
            locals.s.restricted = (locals.feed.restrictedTo != NULL_ID) ? 1 : 0;
            locals.s.timePolicy = locals.feed.definition.timePolicy;
            locals.s.valueType = locals.feed.definition.valueType;
            locals.s.decimals = locals.feed.definition.decimals;
            output.feeds.set(output.count, locals.s);
            output.count++;
        }
    }

    // What an oracle machine calls before serving a FeedRead query for a feed
    // that has a fee or a restriction.
    struct GetAccess_input { uint64 feedId; id user; };
    struct GetAccess_output { bit active; uint16 untilEpoch; };
    struct GetAccess_locals { AccessKey key; Feed feed; };
    PUBLIC_FUNCTION_WITH_LOCALS(GetAccess)
    {
        output.active = false;
        output.untilEpoch = 0;
        if (input.feedId == 0 || input.feedId > state.get().feedCount)
        {
            return;
        }
        locals.feed = state.get().feeds.get(input.feedId - 1);
        if (locals.feed.status != FEEDREG_STATUS_APPROVED)
        {
            return;
        }
        if (locals.feed.feePerEpoch == 0 && locals.feed.restrictedTo == NULL_ID)
        {
            output.active = true;
            output.untilEpoch = 65535;
            return;
        }
        locals.key.user = input.user;
        locals.key.feedId = input.feedId;
        if (state.get().access.get(locals.key, output.untilEpoch))
        {
            output.active = output.untilEpoch >= qpi.epoch();
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
        sint64 proposeDeposit;
        sint64 proposerShareBps;
        sint64 proposalFee;
        sint64 dividendPool;
        sint64 heldDeposits;
        sint64 totalPaidToShareholders;
    };
    PUBLIC_FUNCTION(GetInfo)
    {
        output.feedCount = state.get().feedCount;
        output.proposeDeposit = state.get().proposeDeposit;
        output.proposerShareBps = state.get().proposerShareBps;
        output.proposalFee = state.get().proposalFee;
        output.dividendPool = state.get().dividendPool;
        output.heldDeposits = state.get().heldDeposits;
        output.totalPaidToShareholders = state.get().totalPaidToShareholders;
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
        sint64 elementIndex;
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

        // 3. access fees collected this epoch go to shareholders
        locals.perShare = div<sint64>(state.get().dividendPool, (sint64)NUMBER_OF_COMPUTORS);
        if (locals.perShare > 0 && qpi.distributeDividends(locals.perShare))
        {
            state.mut().dividendPool -= locals.perShare * NUMBER_OF_COMPUTORS;
            state.mut().totalPaidToShareholders += locals.perShare * NUMBER_OF_COMPUTORS;
        }

        // 4. drop access entries that end with this epoch
        locals.elementIndex = state.get().access.nextElementIndex(NULL_INDEX);
        while (locals.elementIndex != NULL_INDEX)
        {
            if (state.get().access.value(locals.elementIndex) <= qpi.epoch())
            {
                state.mut().access.removeByIndex(locals.elementIndex);
            }
            locals.elementIndex = state.get().access.nextElementIndex(locals.elementIndex);
        }
        state.mut().access.cleanupIfNeeded();
        state.mut().liveHashes.cleanupIfNeeded();
    }

    REGISTER_USER_FUNCTIONS_AND_PROCEDURES()
    {
        REGISTER_USER_FUNCTION(GetFeed, 1);
        REGISTER_USER_FUNCTION(ListFeeds, 2);
        REGISTER_USER_FUNCTION(GetAccess, 3);
        REGISTER_USER_FUNCTION(FindByHash, 4);
        REGISTER_USER_FUNCTION(CheckDefinition, 5);
        REGISTER_USER_FUNCTION(GetInfo, 6);

        REGISTER_USER_PROCEDURE(ProposeFeed, 1);
        REGISTER_USER_PROCEDURE(WithdrawFeed, 2);
        REGISTER_USER_PROCEDURE(BuyAccess, 3);

        REGISTER_SHAREHOLDER_PROPOSAL_VOTING();
    }
};
