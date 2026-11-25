// Minimal native mock of the QPI surface used by FeedRegistry.h.
// Runs the contract logic as plain C++ for unit testing outside qubic/core.
// It does NOT replace a GoogleTest suite inside qubic/core.
// K12 is simulated with FNV (same input -> same id); the real chain uses KangarooTwelve.
#pragma once
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <map>
#include <vector>
#include <type_traits>

typedef bool bit;
typedef uint8_t uint8; typedef int8_t sint8;
typedef uint16_t uint16; typedef int16_t sint16;
typedef uint32_t uint32; typedef int32_t sint32;
typedef unsigned long long uint64; typedef long long sint64;

namespace QPI { namespace Ch {
    enum : char
    {
        null = 0, underscore = '_', minus = '-', dot = '.',
        a = 'a', b = 'b', c = 'c', d = 'd', e = 'e', f = 'f', g = 'g', h = 'h', i = 'i', j = 'j', k = 'k', l = 'l', m = 'm',
        n = 'n', o = 'o', p = 'p', q = 'q', r = 'r', s = 's', t = 't', u = 'u', v = 'v', w = 'w', x = 'x', y = 'y', z = 'z',
        _0 = '0', _1 = '1', _2 = '2', _3 = '3', _4 = '4', _5 = '5', _6 = '6', _7 = '7', _8 = '8', _9 = '9',
    };
} }
typedef decltype(QPI::Ch::a) MockChType;

struct id
{
    struct { uint64 _0, _1, _2, _3; } u64;
    id() { u64 = {0, 0, 0, 0}; }
    id(uint64 a, uint64 b, uint64 c, uint64 d) { u64 = {a, b, c, d}; }
    // id(Ch::f, Ch::e, ...) as in core: characters packed from the first byte
    template <typename... T, typename = std::enable_if_t<(std::is_same_v<T, MockChType> && ...)>>
    id(T... cs) { u64 = {0, 0, 0, 0}; char v[] = {cs...}; memcpy(&u64, v, sizeof(v) < 32 ? sizeof(v) : 32); }
    bool operator==(const id& o) const { return !memcmp(&u64, &o.u64, 32); }
    bool operator!=(const id& o) const { return !(*this == o); }
    bool operator<(const id& o) const { return memcmp(&u64, &o.u64, 32) < 0; }
};
static const id NULL_ID(0, 0, 0, 0);
constexpr sint64 NULL_INDEX = -1;
inline uint64 fnv(const void* p, size_t n, uint64 h = 1469598103934665603ULL)
{
    const uint8* b = (const uint8*)p; for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ULL; return h;
}
inline id mockId(const char* s) { uint64 h = fnv(s, strlen(s)); return id(h, h >> 7, h >> 13, 42); }
#define ID(...) mockId(#__VA_ARGS__)

constexpr uint64 NUMBER_OF_COMPUTORS = 676;
constexpr uint8 ORACLE_QUERY_STATUS_UNKNOWN = 0;
constexpr uint8 ORACLE_QUERY_STATUS_PENDING = 1;
constexpr uint8 ORACLE_QUERY_STATUS_COMMITTED = 2;
constexpr uint8 ORACLE_QUERY_STATUS_SUCCESS = 3;
constexpr uint8 ORACLE_QUERY_STATUS_TIMEOUT = 4;
constexpr uint8 ORACLE_QUERY_STATUS_UNRESOLVABLE = 5;

namespace QPI
{
    template <typename T> inline static constexpr T div(T a, T b) { return b ? (a / b) : T(0); }
    template <typename T> inline static constexpr T mod(T a, T b) { return b ? (a % b) : 0; }
    template <typename T> inline void setMemory(T& dst, uint8 v) { memset((void*)&dst, v, sizeof(T)); }

    template <typename T, uint64 L> struct Array
    {
        T _v[L];
        const T& get(uint64 i) const { return _v[i & (L - 1)]; }
        void set(uint64 i, const T& x) { _v[i & (L - 1)] = x; }
        void setAll(const T& x) { for (uint64 i = 0; i < L; i++) _v[i] = x; }
        static constexpr uint64 capacity() { return L; }
    };

    // open addressing with tombstones; zero-initialised memory is a valid empty map
    template <typename K, typename V, uint64 L> struct HashMap
    {
        K _k[L]; V _v[L]; uint8 _f[L]; uint64 _pop;   // _f: 0 empty, 1 used, 2 removed
        uint64 slot(const K& k) const { return fnv(&k, sizeof(K)) & (L - 1); }
        sint64 find(const K& k) const
        {
            uint64 s = slot(k);
            for (uint64 n = 0; n < L; n++, s = (s + 1) & (L - 1)) { if (_f[s] == 0) return -1; if (_f[s] == 1 && !memcmp(&_k[s], &k, sizeof(K))) return (sint64)s; }
            return -1;
        }
        bool contains(const K& k) const { return find(k) >= 0; }
        bool get(const K& k, V& v) const { sint64 i = find(k); if (i < 0) return false; v = _v[i]; return true; }
        sint64 set(const K& k, const V& v)
        {
            sint64 i = find(k); if (i >= 0) { _v[i] = v; return i; }
            if (_pop >= L) return NULL_INDEX;
            uint64 s = slot(k); while (_f[s] == 1) s = (s + 1) & (L - 1);
            _f[s] = 1; _k[s] = k; _v[s] = v; _pop++; return (sint64)s;
        }
        sint64 removeByKey(const K& k) { sint64 i = find(k); if (i >= 0) { _f[i] = 2; _pop--; } return i; }
        void removeByIndex(sint64 i) { if (i >= 0 && _f[i] == 1) { _f[i] = 2; _pop--; } }
        sint64 nextElementIndex(sint64 i) const { for (uint64 j = (i < 0 ? 0 : (uint64)i + 1); j < L; j++) if (_f[j] == 1) return (sint64)j; return NULL_INDEX; }
        const K& key(sint64 i) const { return _k[i]; }
        const V& value(sint64 i) const { return _v[i]; }
        uint64 population() const { return _pop; }
        void cleanupIfNeeded(uint64 = 50) {}
    };

    template <typename S> struct ContractState
    {
        S* s;
        const S& get() const { return *s; }
        S& mut() { return *s; }
    };

    template <typename OI_T> struct OracleNotificationInput
    {
        sint64 queryId;
        sint32 subscriptionId;
        uint8 status;
        uint8 __reserved0;
        uint16 __reserved1;
        typename OI_T::OracleReply reply;
    };
    struct NoData {};
    namespace TransferType
    {
        constexpr uint8 standardTransaction = 0;
        constexpr uint8 procedureTransaction = 1;
        constexpr uint8 qpiTransfer = 2;
        constexpr uint8 qpiDistributeDividends = 3;
        constexpr uint8 revenueDonation = 4;
        constexpr uint8 ipoBidRefund = 5;
        constexpr uint8 procedureInvocationByOtherContract = 6;
    };
    struct PostIncomingTransfer_input { id sourceId; sint64 amount; uint8 type; };
}
using namespace QPI;

namespace QPI
{
    // Same interface as core's DateAndTime for what FeedRegistry uses; stored as microseconds
    // since 0000-03-01 plus one (0 = invalid), so comparisons are plain integer comparisons.
    struct DateAndTime
    {
        uint64 value;
        DateAndTime() : value(0) {}
        DateAndTime(uint64 y, uint64 mo, uint64 d, uint64 h = 0, uint64 mi = 0, uint64 s = 0) : value(0)
        {
            if (isValid(y, mo, d, h, mi, s, 0, 0)) value = ((((days(y, mo, d) * 24 + h) * 60 + mi) * 60 + s) * 1000000ULL) + 1;
        }
        static bool isLeapYear(uint64 y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
        static bool isValid(uint64 y, uint64 mo, uint64 d, uint64 h, uint64 mi, uint64 s, uint64 ms, uint64 us)
        {
            static const uint8 dim[13] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
            if (y > 0xffff || mo < 1 || mo > 12 || d < 1) return false;
            if (d > (uint64)(dim[mo] + (mo == 2 && isLeapYear(y) ? 1 : 0))) return false;
            return h < 24 && mi < 60 && s < 60 && ms < 1000 && us < 1000;
        }
        bool isValid() const { return value != 0; }
        bool setIfValid(uint64 y, uint64 mo, uint64 d, uint64 h, uint64 mi, uint64 s, uint64 ms = 0, uint64 us = 0)
        {
            if (!isValid(y, mo, d, h, mi, s, ms, us)) return false;
            *this = DateAndTime(y, mo, d, h, mi, s); return true;
        }
        static uint64 days(uint64 y, uint64 m, uint64 d)   // days since 0000-03-01
        {
            if (m <= 2) { y -= 1; m += 12; }
            return 365 * y + y / 4 - y / 100 + y / 400 + (153 * (m - 3) + 2) / 5 + d - 1;
        }
        bool add(sint64 years, sint64 months, sint64 dd, sint64 hh, sint64 mm, sint64 ss, sint64 ms = 0, sint64 us = 0)
        {
            if (!value || years || months) return false;
            sint64 delta = ((((dd * 24 + hh) * 60 + mm) * 60 + ss) * 1000 + ms) * 1000 + us;
            if (delta < 0 && (uint64)(-delta) >= value) return false;
            value = (uint64)((sint64)value + delta); return true;
        }
        bool operator<(const DateAndTime& o) const { return value < o.value; }
        bool operator>(const DateAndTime& o) const { return value > o.value; }
        bool operator<=(const DateAndTime& o) const { return value <= o.value; }
        bool operator>=(const DateAndTime& o) const { return value >= o.value; }
        bool operator==(const DateAndTime& o) const { return value == o.value; }
        bool operator!=(const DateAndTime& o) const { return value != o.value; }
    };
}
using QPI::DateAndTime;
namespace OI
{
#define ORACLE_INTERFACE_INDEX 9
#include "../../oracle_interfaces/HttpRead.h"
#undef ORACLE_INTERFACE_INDEX
}
// ---------------------------------------------------------------- shareholder proposals (types as in core qpi_proposals.h)
namespace QPI
{
    constexpr uint16 INVALID_PROPOSAL_INDEX = 0xffff;
    constexpr sint64 NO_VOTE_VALUE = (sint64)0x8000000000000000ULL;
    constexpr uint32 QUORUM = (uint32)(NUMBER_OF_COMPUTORS * 2 / 3 + 1);
    namespace ProposalTypes
    {
        namespace Class
        {
            static constexpr uint16 GeneralOptions = 0;
            static constexpr uint16 Transfer = 0x100;
            static constexpr uint16 Variable = 0x200;
            static constexpr uint16 MultiVariables = 0x300;
            static constexpr uint16 TransferInEpoch = 0x400;
        };
        static constexpr uint16 Invalid = 0;
        static constexpr uint16 YesNo = Class::GeneralOptions | 2;
        static constexpr uint16 TransferYesNo = Class::Transfer | 2;
        static constexpr uint16 VariableYesNo = Class::Variable | 2;
        static constexpr uint16 VariableTwoValues = Class::Variable | 3;
        static uint16 optionCount(uint16 t) { return t & 0x00ff; }
        static uint16 cls(uint16 t) { return t & 0xff00; }
    };
    struct ProposalDataYesNo
    {
        Array<uint8, 256> url;
        uint16 epoch;
        uint16 type;
        uint32 tick;
        union Data
        {
            struct Transfer { id destination; sint64 amount; } transfer;
            struct VariableOptions { uint64 variable; sint64 value; } variableOptions;
            Data() { memset((void*)this, 0, sizeof(*this)); }
        } data;
    };
    struct ProposalMultiVoteDataV1
    {
        uint16 proposalIndex;
        uint16 proposalType;
        uint32 proposalTick;
        Array<sint64, 8> voteValues;
        Array<uint32, 8> voteCounts;
    };
    struct ProposalSummarizedVotingDataV1
    {
        uint16 proposalIndex;
        uint16 optionCount;
        uint32 proposalTick;
        uint32 totalVotesAuthorized;
        uint32 totalVotesCasted;
        union { Array<uint32, 8> optionVoteCount; sint64 scalarVotingResult; };
        sint32 getMostVotedOption() const
        {
            if (optionCount == 0) return -1;
            sint32 best = 0; uint32 bestVotes = optionVoteCount.get(0);
            for (sint32 i = 1; i < optionCount; ++i) if (bestVotes < optionVoteCount.get(i)) { bestVotes = optionVoteCount.get(i); best = i; }
            return best;
        }
        // same rule as core: >= 2/3 of eligible votes cast, winner > 1/3 of eligible votes
        sint32 getAcceptedOption(uint32 totalVotesThresh = QUORUM, uint32 mostVotedThreshold = QUORUM / 2) const
        {
            if (totalVotesCasted >= totalVotesThresh)
            {
                sint32 opt = getMostVotedOption();
                if (opt >= 0 && optionVoteCount.get(opt) > mostVotedThreshold) return opt;
            }
            return -1;
        }
    };
    template <uint16 slots, uint64 assetName> struct ProposalAndVotingByShareholders
    {
        static constexpr uint16 maxProposals = slots;
        static constexpr uint64 asset = assetName;
    };
    typedef Array<uint8, 1024> SET_SHAREHOLDER_PROPOSAL_input;
    typedef uint16 SET_SHAREHOLDER_PROPOSAL_output;
    typedef ProposalMultiVoteDataV1 SET_SHAREHOLDER_VOTES_input;
    typedef bit SET_SHAREHOLDER_VOTES_output;
    template <typename T1, typename T2> inline void copyFromBuffer(T1& dst, const T2& src)
    {
        static_assert(sizeof(T1) <= sizeof(T2), "buffer too small"); memcpy((void*)&dst, (const void*)&src, sizeof(T1));
    }
}


// ---------------------------------------------------------------- ledger / context
struct MockChain
{
    std::map<id, sint64> balances;
    uint32 tick = 1000; uint16 epoch = 250;
    sint64 burned = 0, dividends = 0;
    std::map<id, sint64> shares;      // registry shares possessed (voting rights)
    // ---- simulated HttpRead oracle
    struct MockQuery { sint64 queryId; OI::HttpRead::OracleQuery query; uint32 timeoutMs; };
    std::vector<MockQuery> queries;
    sint64 nextQueryId = 5000;
    sint64 oracleFeesPaid = 0;
    bool oracleRejects = false;
    DateAndTime now = DateAndTime(2026, 11, 24, 12, 0, 0);
};
inline MockChain& chain() { static MockChain c; return c; }

// Mock of core's ProposalVoting<ProposalAndVotingByShareholders, ProposalDataYesNo>:
// shareholder-only proposing, one proposal per proposer, voting rights = shares possessed
// when the proposal is set, one vote per share, a voter may split shares between options.
template <typename PV, typename D> struct ProposalVoting
{
    static constexpr uint16 N = PV::maxProposals;
    static constexpr uint32 MAXV = (uint32)NUMBER_OF_COMPUTORS;
    struct Slot
    {
        id proposer; D data;
        id voter[MAXV]; uint32 shares[MAXV]; uint32 count;
        uint32 votesFor[MAXV][2];
    } slot[N];

    bool active(uint16 i) const { return i < N && slot[i].proposer != NULL_ID && slot[i].data.epoch == chain().epoch; }
    uint16 setProposal(const id& proposer, const D& in)
    {
        if (proposer == NULL_ID || chain().shares[proposer] <= 0) return INVALID_PROPOSAL_INDEX;
        uint16 idx = INVALID_PROPOSAL_INDEX;
        for (uint16 i = 0; i < N; i++) if (slot[i].proposer == proposer && slot[i].data.epoch == chain().epoch) idx = i;
        if (in.epoch == 0)
        {
            if (idx == INVALID_PROPOSAL_INDEX) return INVALID_PROPOSAL_INDEX;
            memset((void*)&slot[idx], 0, sizeof(Slot)); return idx;
        }
        if (in.epoch != chain().epoch) return INVALID_PROPOSAL_INDEX;
        if (ProposalTypes::cls(in.type) == ProposalTypes::Class::Variable && ProposalTypes::optionCount(in.type) != 2) return INVALID_PROPOSAL_INDEX;
        if (idx == INVALID_PROPOSAL_INDEX)
            for (uint16 i = 0; i < N && idx == INVALID_PROPOSAL_INDEX; i++) if (!active(i)) idx = i;
        if (idx == INVALID_PROPOSAL_INDEX) return INVALID_PROPOSAL_INDEX;
        Slot& s = slot[idx]; memset((void*)&s, 0, sizeof(Slot));
        s.proposer = proposer; s.data = in; s.data.tick = chain().tick;
        for (auto& kv : chain().shares) if (kv.second > 0 && s.count < MAXV) { s.voter[s.count] = kv.first; s.shares[s.count] = (uint32)kv.second; s.count++; }
        return idx;
    }
    bit vote(const id& voter, const ProposalMultiVoteDataV1& v)
    {
        uint16 i = v.proposalIndex; if (!active(i)) return false;
        Slot& s = slot[i];
        if (v.proposalType != s.data.type || v.proposalTick != s.data.tick) return false;
        for (uint32 k = 0; k < s.count; k++) if (s.voter[k] == voter)
        {
            uint32 c[2] = {0, 0}, total = 0; bool counts = false;
            for (int j = 0; j < 8; j++) if (v.voteCounts.get(j)) counts = true;
            for (int j = 0; j < 8; j++)
            {
                uint32 n = counts ? v.voteCounts.get(j) : (j == 0 ? s.shares[k] : 0);
                if (!n) continue;
                sint64 val = v.voteValues.get(j);
                if (val == NO_VOTE_VALUE) { total += n; continue; }
                if (val < 0 || val > 1) return false;
                c[val] += n; total += n;
            }
            if (total > s.shares[k]) return false;
            s.votesFor[k][0] = c[0]; s.votesFor[k][1] = c[1]; return true;
        }
        return false;
    }
    bool getProposal(uint16 i, D& out) const { if (i >= N || slot[i].proposer == NULL_ID) return false; out = slot[i].data; return true; }
    id proposerId(uint16 i) const { return i < N ? slot[i].proposer : NULL_ID; }
    sint32 nextProposalIndex(sint32 prev, uint16 epoch) const
    {
        for (sint32 i = prev + 1; i < (sint32)N; i++) if (slot[i].proposer != NULL_ID && slot[i].data.epoch == epoch) return i;
        return -1;
    }
    sint32 nextFinishedProposalIndex(sint32 prev) const
    {
        for (sint32 i = prev + 1; i < (sint32)N; i++) if (slot[i].proposer != NULL_ID && slot[i].data.epoch && slot[i].data.epoch < chain().epoch) return i;
        return -1;
    }
    bool getVotingSummary(uint16 i, ProposalSummarizedVotingDataV1& out) const
    {
        if (i >= N || slot[i].proposer == NULL_ID) return false;
        memset((void*)&out, 0, sizeof(out));
        out.proposalIndex = i; out.optionCount = 2; out.proposalTick = slot[i].data.tick; out.totalVotesAuthorized = MAXV;
        uint32 no = 0, yes = 0; for (uint32 k = 0; k < slot[i].count; k++) { no += slot[i].votesFor[k][0]; yes += slot[i].votesFor[k][1]; }
        out.optionVoteCount.set(0, no); out.optionVoteCount.set(1, yes); out.totalVotesCasted = no + yes;
        return true;
    }
    bool getVotes(uint16 i, const id& voter, ProposalMultiVoteDataV1& out) const
    {
        memset((void*)&out, 0, sizeof(out));
        if (i >= N || slot[i].proposer == NULL_ID) return false;
        for (uint32 k = 0; k < slot[i].count; k++) if (slot[i].voter[k] == voter)
        { out.proposalIndex = i; out.proposalType = slot[i].data.type; out.proposalTick = slot[i].data.tick;
          out.voteValues.set(0, 0); out.voteCounts.set(0, slot[i].votesFor[k][0]); out.voteValues.set(1, 1); out.voteCounts.set(1, slot[i].votesFor[k][1]); return true; }
        return false;
    }
};

static const id MOCK_CONTRACT_ID(32, 0, 0, 0);
#define SELF MOCK_CONTRACT_ID
namespace QPI { struct Entity { id publicKey; sint64 incomingAmount, outgoingAmount; }; }

struct QpiCtx
{
    id _invocator; sint64 _reward;
    id invocator() const { return _invocator; }
    id originator() const { return _invocator; }
    sint64 invocationReward() const { return _reward; }
    sint64 transfer(const id& to, sint64 amt) const
    {
        auto& b = chain().balances;
        if (amt < 0 || b[MOCK_CONTRACT_ID] < amt) { fprintf(stderr, "MOCK: transfer of %lld exceeds contract balance %lld\n", amt, b[MOCK_CONTRACT_ID]); abort(); }
        b[MOCK_CONTRACT_ID] -= amt; b[to] += amt; return b[MOCK_CONTRACT_ID];
    }
    sint64 burn(sint64 amt) const
    {
        auto& b = chain().balances; if (b[MOCK_CONTRACT_ID] < amt) return -amt;
        b[MOCK_CONTRACT_ID] -= amt; chain().burned += amt; return b[MOCK_CONTRACT_ID];
    }
    bool distributeDividends(sint64 perShare) const
    {
        auto& b = chain().balances; sint64 t = perShare * (sint64)NUMBER_OF_COMPUTORS;
        if (b[MOCK_CONTRACT_ID] < t) return false;
        b[MOCK_CONTRACT_ID] -= t; chain().dividends += t; return true;
    }
    template <typename T> id K12(const T& data) const
    {
        uint64 h = fnv(&data, sizeof(T));
        return id(h, fnv(&data, sizeof(T), h), fnv(&data, sizeof(T), h ^ 0x9e3779b97f4a7c15ULL), 7);
    }
    bool getEntity(const id& who, Entity& e) const { memset(&e, 0, sizeof(e)); e.publicKey = who; e.incomingAmount = chain().balances[who]; return true; }
    uint32 tick() const { return chain().tick; }
    uint16 epoch() const { return chain().epoch; }
    DateAndTime now() const { return chain().now; }
    sint64 queryOracle(const OI::HttpRead::OracleQuery& q, uint32 timeoutMs) const
    {
        sint64 fee = OI::HttpRead::getQueryFee(q);
        auto& b = chain().balances;
        if (chain().oracleRejects || b[MOCK_CONTRACT_ID] < fee) return -1;
        b[MOCK_CONTRACT_ID] -= fee; chain().oracleFeesPaid += fee;
        sint64 qid = chain().nextQueryId++;
        chain().queries.push_back({qid, q, timeoutMs});
        return qid;
    }
    template <typename T> T& operator()(T& x) const { return x; }
    template <typename T> const T& operator()(const T& x) const { return x; }
};

struct ContractBase {};
#define SELF_INDEX 32
#define LOG_INFO(x) ((void)(x))

#define QUERY_ORACLE(OracleInterface, query, userProcNotification, timeoutMillisec) qpi.queryOracle(query, timeoutMillisec)
#define SUBSCRIBE_ORACLE(OracleInterface, query, userProcNotification, periodMs, notifyWithPrevious) qpi.subscribeOracle(query, periodMs)
#define BEGIN_EPOCH() static void beginEpoch(const QpiCtx& qpi, ContractState<StateData>& state)
#define CALL(fn, in, out) do { fn##_locals* __l = (fn##_locals*)calloc(1, sizeof(fn##_locals)); fn(qpi, state, in, out, *__l); free(__l); } while (0)

#define PUBLIC_PROCEDURE_WITH_LOCALS(n) static void n(const QpiCtx& qpi, ContractState<StateData>& state, const n##_input& input, n##_output& output, n##_locals& locals)
#define PUBLIC_PROCEDURE(n) struct n##_locals {}; PUBLIC_PROCEDURE_WITH_LOCALS(n)
#define PRIVATE_PROCEDURE_WITH_LOCALS(n) static void n(const QpiCtx& qpi, ContractState<StateData>& state, n##_input& input, n##_output& output, n##_locals& locals)
#define PUBLIC_FUNCTION_WITH_LOCALS(n) static void n(const QpiCtx& qpi, ContractState<StateData>& state, n##_input& input, n##_output& output, n##_locals& locals)
#define PUBLIC_FUNCTION(n) struct n##_locals {}; PUBLIC_FUNCTION_WITH_LOCALS(n)
#define PRIVATE_FUNCTION_WITH_LOCALS(n) PUBLIC_FUNCTION_WITH_LOCALS(n)
#define PRIVATE_FUNCTION(n) PUBLIC_FUNCTION(n)
#define REGISTER_USER_FUNCTIONS_AND_PROCEDURES() static void registerAll()
#define REGISTER_USER_FUNCTION(n, i) {}
#define REGISTER_USER_PROCEDURE(n, i) {}
#define REGISTER_USER_PROCEDURE_NOTIFICATION(n) ((void)0)
#define POST_INCOMING_TRANSFER() static void postIncomingTransfer(const QpiCtx& qpi, ContractState<StateData>& state, const PostIncomingTransfer_input& input)
#define INITIALIZE() static void initialize(const QpiCtx& qpi, ContractState<StateData>& state)
#define INITIALIZE_WITH_LOCALS() static void initialize(const QpiCtx& qpi, ContractState<StateData>& state, INITIALIZE_locals& locals)
#define PRIVATE_PROCEDURE(n) struct n##_locals {}; PRIVATE_PROCEDURE_WITH_LOCALS(n)
#define SET_SHAREHOLDER_PROPOSAL_WITH_LOCALS() static void setShareholderProposalSys(const QpiCtx& qpi, ContractState<StateData>& state, SET_SHAREHOLDER_PROPOSAL_input& input, SET_SHAREHOLDER_PROPOSAL_output& output, SET_SHAREHOLDER_PROPOSAL_locals& locals)
#define SET_SHAREHOLDER_VOTES() struct SET_SHAREHOLDER_VOTES_locals {}; static void setShareholderVotesSys(const QpiCtx& qpi, ContractState<StateData>& state, SET_SHAREHOLDER_VOTES_input& input, SET_SHAREHOLDER_VOTES_output& output, SET_SHAREHOLDER_VOTES_locals& locals)

// ---- shareholder proposal macros, copied verbatim from qubic/core src/qpi/qpi_macros.h
#define DEFINE_SHAREHOLDER_PROPOSAL_TYPES(numProposalSlots, assetNameInt64) \
		public: \
			typedef ProposalDataYesNo ProposalDataT; \
			typedef ProposalAndVotingByShareholders<numProposalSlots, assetNameInt64> ProposersAndVotersT; \
			typedef ProposalVoting<ProposersAndVotersT, ProposalDataT> ProposalVotingT

#define IMPLEMENT_SetShareholderProposal(numFeeStateVariables, setProposalFeeVarOrValue) \
		typedef ProposalDataT SetShareholderProposal_input; \
		typedef uint16 SetShareholderProposal_output; \
		PUBLIC_PROCEDURE(SetShareholderProposal) { \
			if (qpi.invocationReward() < setProposalFeeVarOrValue || (input.epoch \
				&& (input.type != ProposalTypes::VariableYesNo || input.data.variableOptions.variable >= numFeeStateVariables \
					|| input.data.variableOptions.value < 0))) { \
				qpi.transfer(qpi.invocator(), qpi.invocationReward()); \
				output = INVALID_PROPOSAL_INDEX; \
				return; } \
			output = qpi(state.mut().proposals).setProposal(qpi.invocator(), input); \
			if (output == INVALID_PROPOSAL_INDEX) { \
				qpi.transfer(qpi.invocator(), qpi.invocationReward()); \
				return;	} \
			qpi.burn(setProposalFeeVarOrValue); \
			if (qpi.invocationReward() > setProposalFeeVarOrValue) { \
				qpi.transfer(qpi.invocator(), qpi.invocationReward() - setProposalFeeVarOrValue); } }

#define IMPLEMENT_GetShareholderProposal() \
		struct GetShareholderProposal_input { uint16 proposalIndex; }; \
		struct GetShareholderProposal_output { ProposalDataT proposal; id proposerPubicKey; }; \
		PUBLIC_FUNCTION(GetShareholderProposal) { \
			output.proposerPubicKey = qpi(state.get().proposals).proposerId(input.proposalIndex); \
			qpi(state.get().proposals).getProposal(input.proposalIndex, output.proposal); }

#define IMPLEMENT_GetShareholderProposalIndices() \
		struct GetShareholderProposalIndices_input { bit activeProposals; sint32 prevProposalIndex; }; \
		struct GetShareholderProposalIndices_output { uint16 numOfIndices; Array<uint16, 64> indices; }; \
		PUBLIC_FUNCTION(GetShareholderProposalIndices) {\
			if (input.activeProposals) { \
				while ((input.prevProposalIndex = qpi(state.get().proposals).nextProposalIndex(input.prevProposalIndex, qpi.epoch())) >= 0) { \
					output.indices.set(output.numOfIndices, input.prevProposalIndex); \
					++output.numOfIndices; \
					if (output.numOfIndices == output.indices.capacity()) break; } } \
			else { \
				while ((input.prevProposalIndex = qpi(state.get().proposals).nextFinishedProposalIndex(input.prevProposalIndex)) >= 0) { \
					output.indices.set(output.numOfIndices, input.prevProposalIndex); \
					++output.numOfIndices; \
					if (output.numOfIndices == output.indices.capacity()) break; } } }

#define IMPLEMENT_GetShareholderProposalFees(setProposalFeeVarOrValue) \
		typedef NoData GetShareholderProposalFees_input; \
		struct GetShareholderProposalFees_output { sint64 setProposalFee; sint64 setVoteFee; }; \
		PUBLIC_FUNCTION(GetShareholderProposalFees) { \
			output.setProposalFee = setProposalFeeVarOrValue; \
			output.setVoteFee = 0; }

#define IMPLEMENT_SetShareholderVotes() \
		typedef ProposalMultiVoteDataV1 SetShareholderVotes_input; \
		typedef bit SetShareholderVotes_output; \
		PUBLIC_PROCEDURE(SetShareholderVotes) { \
			output = qpi(state.mut().proposals).vote(qpi.invocator(), input); } \

#define IMPLEMENT_GetShareholderVotes() \
		struct GetShareholderVotes_input { id voter; uint16 proposalIndex; }; \
		typedef ProposalMultiVoteDataV1 GetShareholderVotes_output; \
		PUBLIC_FUNCTION(GetShareholderVotes) { \
			qpi(state.get().proposals).getVotes(input.proposalIndex, input.voter,	output); }

#define IMPLEMENT_GetShareholderVotingResults() \
		struct GetShareholderVotingResults_input { uint16 proposalIndex; }; \
		typedef ProposalSummarizedVotingDataV1 GetShareholderVotingResults_output; \
		PUBLIC_FUNCTION(GetShareholderVotingResults) { \
			qpi(state.get().proposals).getVotingSummary(input.proposalIndex, output); }

#define IMPLEMENT_SET_SHAREHOLDER_PROPOSAL() \
		struct SET_SHAREHOLDER_PROPOSAL_locals { SetShareholderProposal_input userProcInput; }; \
		SET_SHAREHOLDER_PROPOSAL_WITH_LOCALS() { \
			copyFromBuffer(locals.userProcInput, input); \
			CALL(SetShareholderProposal, locals.userProcInput, output); }

#define IMPLEMENT_SET_SHAREHOLDER_VOTES() \
		SET_SHAREHOLDER_VOTES() { \
			CALL(SetShareholderVotes, input, output); }

	// Define procedures for easily implementing END_EPOCH
#define IMPLEMENT_FinalizeShareholderStateVarProposals() \
		struct FinalizeShareholderProposalSetStateVar_input { \
			sint32 proposalIndex; ProposalDataT proposal; ProposalSummarizedVotingDataV1 results; \
			sint32 acceptedOption; 	sint64 acceptedValue; }; \
		typedef NoData FinalizeShareholderProposalSetStateVar_output; \
		typedef NoData FinalizeShareholderStateVarProposals_input; \
		typedef NoData FinalizeShareholderStateVarProposals_output; \
		struct FinalizeShareholderStateVarProposals_locals { \
			FinalizeShareholderProposalSetStateVar_input p; uint16 proposalClass; }; \
		PRIVATE_PROCEDURE_WITH_LOCALS(FinalizeShareholderStateVarProposals) { \
			locals.p.proposalIndex = -1; \
			while ((locals.p.proposalIndex = qpi(state.get().proposals).nextProposalIndex(locals.p.proposalIndex, qpi.epoch())) >= 0) { \
				if (!qpi(state.get().proposals).getProposal(locals.p.proposalIndex, locals.p.proposal)) \
					continue; \
				locals.proposalClass = ProposalTypes::cls(locals.p.proposal.type); \
				if (locals.proposalClass == ProposalTypes::Class::Variable || locals.proposalClass == ProposalTypes::Class::MultiVariables) { \
					if (!qpi(state.get().proposals).getVotingSummary(locals.p.proposalIndex, locals.p.results)) \
						continue; \
					locals.p.acceptedOption = locals.p.results.getAcceptedOption(); \
					if (locals.p.acceptedOption <= 0) \
						continue; \
					locals.p.acceptedValue = locals.p.proposal.data.variableOptions.value; \
					CALL(FinalizeShareholderProposalSetStateVar, locals.p, output); } } } \
		PRIVATE_PROCEDURE(FinalizeShareholderProposalSetStateVar)

#define IMPLEMENT_DEFAULT_SHAREHOLDER_PROPOSAL_VOTING(numFeeStateVariables, setProposalFeeVarOrValue) \
		IMPLEMENT_SetShareholderProposal(numFeeStateVariables, setProposalFeeVarOrValue) \
		IMPLEMENT_GetShareholderProposal() \
		IMPLEMENT_GetShareholderProposalIndices() \
		IMPLEMENT_GetShareholderProposalFees(setProposalFeeVarOrValue) \
		IMPLEMENT_SetShareholderVotes() \
		IMPLEMENT_GetShareholderVotes() \
		IMPLEMENT_GetShareholderVotingResults() \
		IMPLEMENT_SET_SHAREHOLDER_PROPOSAL() \
		IMPLEMENT_SET_SHAREHOLDER_VOTES()

#define REGISTER_GetShareholderProposalFees() REGISTER_USER_FUNCTION(GetShareholderProposalFees, 65531)
#define REGISTER_GetShareholderProposalIndices() REGISTER_USER_FUNCTION(GetShareholderProposalIndices, 65532)
#define REGISTER_GetShareholderProposal() REGISTER_USER_FUNCTION(GetShareholderProposal, 65533)
#define REGISTER_GetShareholderVotes() REGISTER_USER_FUNCTION(GetShareholderVotes, 65534)
#define REGISTER_GetShareholderVotingResults() REGISTER_USER_FUNCTION(GetShareholderVotingResults, 65535)
#define REGISTER_SetShareholderProposal() REGISTER_USER_PROCEDURE(SetShareholderProposal, 65534)
#define REGISTER_SetShareholderVotes() REGISTER_USER_PROCEDURE(SetShareholderVotes, 65535)

#define REGISTER_SHAREHOLDER_PROPOSAL_VOTING()  REGISTER_GetShareholderProposalFees() \
		REGISTER_GetShareholderProposalIndices(); REGISTER_GetShareholderProposal(); \
		REGISTER_GetShareholderVotes(); REGISTER_GetShareholderVotingResults(); \
		REGISTER_SetShareholderProposal(); REGISTER_SetShareholderVotes()

#define END_EPOCH_WITH_LOCALS() static void endEpoch(const QpiCtx& qpi, ContractState<StateData>& state, END_EPOCH_locals& locals)
