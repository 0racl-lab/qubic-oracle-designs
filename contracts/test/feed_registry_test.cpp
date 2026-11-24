// Native tests for FeedRegistry.h against mock_qpi.h.
#include <gtest/gtest.h>
#include <string>
#include <vector>
#include "mock_qpi.h"
#include "FeedRegistry.h"

typedef FEEDREG R;

static const id ALICE = id(11, 1, 1, 1), BOB = id(22, 2, 2, 2), CAROL = id(33, 3, 3, 3);
static const id SH1 = id(101, 0, 0, 0), SH2 = id(102, 0, 0, 0), CONSUMER = id(201, 0, 0, 0), CONSUMER2 = id(202, 0, 0, 0);

template <uint64 N> static void put(Array<uint8, N>& a, const std::string& s)
{
    a.setAll(0);
    for (size_t i = 0; i < s.size() && i < N; i++) a.set(i, (uint8)s[i]);
}

// The Frankfurter example from designs/http-read.md: EUR->USD rate for a date, 6 decimals.
static R::FeedDefinition frankfurter()
{
    R::FeedDefinition d; memset(&d, 0, sizeof(d));
    put(d.urlTemplate, "https://api.frankfurter.app/{0}?from={1}&to={2}");
    put(d.valuePath, "/rates/{2}");
    d.paramKinds.set(0, FEEDREG_PARAM_ISO_DATE);
    d.paramKinds.set(1, FEEDREG_PARAM_CURRENCY);
    d.paramKinds.set(2, FEEDREG_PARAM_CURRENCY);
    d.timePolicy = FEEDREG_TIME_HISTORICAL;
    d.minAgeSeconds = 86400;
    d.valueType = OI::HttpRead::ValueType::decimal;
    d.decimals = 6;
    return d;
}
static R::FeedDefinition withUrl(const std::string& url, int params = 0)
{
    R::FeedDefinition d = frankfurter();
    put(d.urlTemplate, url);
    put(d.valuePath, "/value");
    d.paramKinds.setAll(0);
    for (int i = 0; i < params; i++) d.paramKinds.set(i, FEEDREG_PARAM_SLUG);
    return d;
}
static R::FeedDefinition withValuePath(const std::string& p)
{
    R::FeedDefinition d = frankfurter(); put(d.valuePath, p); return d;
}
static sint64 bal(const id& u) { return chain().balances[u]; }

struct H
{
    R::StateData* s;
    ContractState<R::StateData> cs;
    H()
    {
        chain() = MockChain();
        s = (R::StateData*)calloc(1, sizeof(R::StateData));
        cs.s = s;
        QpiCtx q{NULL_ID, 0};
        R::initialize(q, cs);
        chain().shares[SH1] = 400;
        chain().shares[SH2] = 276;
        for (const id& u : {ALICE, BOB, CAROL, SH1, SH2, CONSUMER, CONSUMER2}) chain().balances[u] = 1000000000000LL;
    }
    ~H() { free(s); }

#define P(NAME) \
    R::NAME##_output NAME(const R::NAME##_input& in, const id& user, sint64 amount = 0) \
    { \
        R::NAME##_output out; memset(&out, 0xee, sizeof(out)); \
        chain().balances[user] -= amount; chain().balances[MOCK_CONTRACT_ID] += amount; \
        QpiCtx q{user, amount}; auto* l = (R::NAME##_locals*)calloc(1, sizeof(R::NAME##_locals)); R::NAME(q, cs, in, out, *l); free(l); return out; \
    }
#define F(NAME) \
    R::NAME##_output NAME(const R::NAME##_input& in) \
    { \
        R::NAME##_output out; memset(&out, 0, sizeof(out)); \
        QpiCtx q{NULL_ID, 0}; auto inCopy = in; auto* l = (R::NAME##_locals*)calloc(1, sizeof(R::NAME##_locals)); R::NAME(q, cs, inCopy, out, *l); free(l); return out; \
    }
    P(ProposeFeed) P(WithdrawFeed) P(RequestValue) P(SetShareholderProposal) P(SetShareholderVotes)
    F(GetFeed) F(ListFeeds) F(GetRequest) F(FindByHash) F(CheckDefinition) F(GetInfo) F(GetShareholderProposal)

    uint32 check(const R::FeedDefinition& d) { R::CheckDefinition_input in{}; in.definition = d; return CheckDefinition(in).returnCode; }

    R::ProposeFeed_output propose(const R::FeedDefinition& d, const id& who = ALICE, sint64 fee = 0, const id& restrictedTo = NULL_ID, sint64 pay = FEEDREG_DEFAULT_DEPOSIT)
    {
        R::ProposeFeed_input in; memset(&in, 0, sizeof(in));
        in.definition = d; in.feePerUse = fee; in.restrictedTo = restrictedTo;
        return ProposeFeed(in, who, pay);
    }
    R::Feed feed(uint64 feedId) { R::GetFeed_input in{feedId}; auto o = GetFeed(in); EXPECT_EQ(o.returnCode, FEEDREG_OK); return o.feed; }
    uint32 status(uint64 feedId) { return feed(feedId).status; }

    uint16 openVote(uint64 variable, sint64 value, const id& who = SH1, sint64 pay = FEEDREG_DEFAULT_PROPOSAL_FEE)
    {
        R::SetShareholderProposal_input in; memset((void*)&in, 0, sizeof(in));
        in.epoch = chain().epoch; in.type = ProposalTypes::VariableYesNo;
        in.data.variableOptions.variable = variable; in.data.variableOptions.value = value;
        return SetShareholderProposal(in, who, pay);
    }
    bool vote(const id& who, uint16 idx, sint64 option)
    {
        R::GetShareholderProposal_input gi{}; gi.proposalIndex = idx; auto pr = GetShareholderProposal(gi);
        ProposalMultiVoteDataV1 v; memset(&v, 0, sizeof(v));
        v.proposalIndex = idx; v.proposalType = pr.proposal.type; v.proposalTick = pr.proposal.tick; v.voteValues.set(0, option);
        return SetShareholderVotes(v, who);
    }
    // open a vote and have both shareholders vote yes (accepted at END_EPOCH)
    void pass(uint64 variable, sint64 value)
    {
        uint16 idx = openVote(variable, value);
        ASSERT_NE(idx, INVALID_PROPOSAL_INDEX);
        ASSERT_TRUE(vote(SH1, idx, 1));
        ASSERT_TRUE(vote(SH2, idx, 1));
    }
    void endEpoch()
    {
        QpiCtx q{NULL_ID, 0}; auto* l = (R::END_EPOCH_locals*)calloc(1, sizeof(R::END_EPOCH_locals));
        R::endEpoch(q, cs, *l); free(l); chain().epoch++;
    }
    uint64 approved(const R::FeedDefinition& d, sint64 fee = 0, const id& restrictedTo = NULL_ID)
    {
        auto o = propose(d, ALICE, fee, restrictedTo);
        EXPECT_EQ(o.returnCode, FEEDREG_OK);
        pass(FEEDREG_VAR_APPROVE_FEED, (sint64)o.feedId);
        endEpoch();
        EXPECT_EQ(status(o.feedId), FEEDREG_STATUS_APPROVED);
        return o.feedId;
    }
    R::RequestValue_output request(uint64 feedId, const DateAndTime& when, const std::vector<std::string>& params, const id& who = CONSUMER, sint64 pay = -1)
    {
        R::RequestValue_input in; memset((void*)&in, 0, sizeof(in));
        in.feedId = feedId; in.timestamp = when;
        for (size_t j = 0; j < params.size() && j < 4; j++)
            for (size_t i = 0; i < params[j].size() && i < 32; i++) in.params.set(j * 32 + i, (uint8)params[j][i]);
        if (pay < 0) { R::GetFeed_input gi{feedId}; pay = GetFeed(gi).requestCost; }
        return RequestValue(in, who, pay);
    }
    R::Request req(uint64 requestId) { R::GetRequest_input in{requestId}; auto o = GetRequest(in); EXPECT_EQ(o.returnCode, FEEDREG_OK); return o.request; }
    // the oracle engine calling back
    void notify(sint64 queryId, uint8 status, sint64 value = 0, uint64 code = OI::HttpRead::RESULT_SUCCESS)
    {
        R::NotifyHttpRead_input in; memset((void*)&in, 0, sizeof(in));
        in.queryId = queryId; in.subscriptionId = -1; in.status = status;
        if (status == ORACLE_QUERY_STATUS_SUCCESS) { in.reply.code = code; in.reply.valueType = OI::HttpRead::ValueType::decimal; in.reply.intValue = code ? 0 : value; }
        R::NotifyHttpRead_output out;
        QpiCtx q{NULL_ID, 0}; auto* l = (R::NotifyHttpRead_locals*)calloc(1, sizeof(R::NotifyHttpRead_locals)); R::NotifyHttpRead(q, cs, in, out, *l); free(l);
    }
    sint64 lastQueryId() { return chain().queries.back().queryId; }

    // the contract never holds QU it cannot account for
    void invariant()
    {
        EXPECT_EQ(bal(MOCK_CONTRACT_ID), s->heldDeposits + s->heldFees + s->dividendPool);
        sint64 fees = 0;
        for (uint64 i = 0; i < FEEDREG_MAX_REQUESTS; i++)
        {
            R::Request r = s->requests.get(i);
            if (r.status == FEEDREG_REQUEST_PENDING) fees += r.feeHeld; else EXPECT_EQ(r.feeHeld, 0);
        }
        EXPECT_EQ(fees, s->heldFees);
        sint64 held = 0;
        for (uint64 i = 0; i < s->feedCount; i++)
        {
            R::Feed f = s->feeds.get(i);
            if (f.status == FEEDREG_STATUS_PENDING) held += f.deposit; else EXPECT_EQ(f.deposit, 0);
        }
        EXPECT_EQ(held, s->heldDeposits);
    }
};

// ============================================================ definition validation

TEST(Definition, FrankfurterIsValidAndHashIsStable)
{
    H h;
    R::CheckDefinition_input in{}; in.definition = frankfurter();
    auto a = h.CheckDefinition(in), b = h.CheckDefinition(in);
    EXPECT_EQ(a.returnCode, FEEDREG_OK);
    EXPECT_NE(a.definitionHash, NULL_ID);
    EXPECT_EQ(a.definitionHash, b.definitionHash);
    in.definition.minAgeSeconds++;
    EXPECT_NE(h.CheckDefinition(in).definitionHash, a.definitionHash);
}

TEST(Definition, CheckReturnsNoHashOnError)
{
    H h;
    R::CheckDefinition_input in{}; in.definition = withUrl("http://example.com/x");
    auto o = h.CheckDefinition(in);
    EXPECT_EQ(o.returnCode, FEEDREG_ERR_URL_SCHEME);
    EXPECT_EQ(o.definitionHash, NULL_ID);
}

TEST(Definition, SchemeMustBeHttps)
{
    H h;
    EXPECT_EQ(h.check(withUrl("http://example.com/x")), FEEDREG_ERR_URL_SCHEME);
    EXPECT_EQ(h.check(withUrl("HTTPS://example.com/x")), FEEDREG_ERR_URL_SCHEME);
    EXPECT_EQ(h.check(withUrl("https:/example.com/x")), FEEDREG_ERR_URL_SCHEME);
    EXPECT_EQ(h.check(withUrl("ftp://example.com/x")), FEEDREG_ERR_URL_SCHEME);
    EXPECT_EQ(h.check(withUrl("httpx://example.com/x")), FEEDREG_ERR_URL_SCHEME);
    EXPECT_EQ(h.check(withUrl("https//:example.com/x")), FEEDREG_ERR_URL_SCHEME);
    EXPECT_EQ(h.check(withUrl("")), FEEDREG_ERR_URL_SCHEME);
    EXPECT_EQ(h.check(withUrl("https://example.com/x")), FEEDREG_OK);
}

TEST(Definition, HostMustBeALowerCaseDnsName)
{
    H h;
    EXPECT_EQ(h.check(withUrl("https://a.io/")), FEEDREG_OK);
    EXPECT_EQ(h.check(withUrl("https://api-1.data.example.org/x")), FEEDREG_OK);
    EXPECT_EQ(h.check(withUrl("https://1.2.3.4/x")), FEEDREG_ERR_URL_HOST);           // IPv4 literal
    EXPECT_EQ(h.check(withUrl("https://[::1]/x")), FEEDREG_ERR_URL_HOST);             // IPv6 literal
    EXPECT_EQ(h.check(withUrl("https://localhost/x")), FEEDREG_ERR_URL_HOST);         // no dot
    EXPECT_EQ(h.check(withUrl("https://Example.com/x")), FEEDREG_ERR_URL_HOST);       // upper case
    EXPECT_EQ(h.check(withUrl("https://user@example.com/x")), FEEDREG_ERR_URL_HOST);  // userinfo
    EXPECT_EQ(h.check(withUrl("https://example.com:8443/x")), FEEDREG_ERR_URL_HOST);  // port
    EXPECT_EQ(h.check(withUrl("https://.example.com/x")), FEEDREG_ERR_URL_HOST);
    EXPECT_EQ(h.check(withUrl("https://example..com/x")), FEEDREG_ERR_URL_HOST);
    EXPECT_EQ(h.check(withUrl("https://example.com./x")), FEEDREG_ERR_URL_HOST);
    EXPECT_EQ(h.check(withUrl("https://-example.com/x")), FEEDREG_ERR_URL_HOST);
    EXPECT_EQ(h.check(withUrl("https://example-.com/x")), FEEDREG_ERR_URL_HOST);
    EXPECT_EQ(h.check(withUrl("https://a.-b.com/x")), FEEDREG_ERR_URL_HOST);
    EXPECT_EQ(h.check(withUrl("https://example.c/x")), FEEDREG_ERR_URL_HOST);         // one-letter TLD
    EXPECT_EQ(h.check(withUrl("https://example.c0m/x")), FEEDREG_ERR_URL_HOST);       // digit in TLD
    EXPECT_EQ(h.check(withUrl("https://exa_mple.com/x")), FEEDREG_ERR_URL_HOST);
    EXPECT_EQ(h.check(withUrl("https://a.b/x")), FEEDREG_ERR_URL_HOST);               // too short
    EXPECT_EQ(h.check(withUrl("https://abcd/x")), FEEDREG_ERR_URL_HOST);
    EXPECT_EQ(h.check(withUrl("https://example.co-/x")), FEEDREG_ERR_URL_HOST);
}

TEST(Definition, HostRunningToTheEndIsRejected)
{
    H h;
    std::string url = "https://" + std::string(240, 'a') + ".com";   // no path, fills the buffer
    EXPECT_EQ(h.check(withUrl(url)), FEEDREG_ERR_URL_PATH);
}

TEST(Definition, PathAndQueryCharacters)
{
    H h;
    EXPECT_EQ(h.check(withUrl("https://example.com")), FEEDREG_ERR_URL_PATH);              // no path
    EXPECT_EQ(h.check(withUrl("https://example.com?x=1")), FEEDREG_ERR_URL_PATH);          // query without path
    EXPECT_EQ(h.check(withUrl("https://example.com/a/b-c_d.e~f?x=1&y=2,3:4+5")), FEEDREG_OK);
    EXPECT_EQ(h.check(withUrl("https://example.com/a%2Fb%2f")), FEEDREG_OK);
    EXPECT_EQ(h.check(withUrl("https://example.com/a%2")), FEEDREG_ERR_URL_PATH);
    EXPECT_EQ(h.check(withUrl("https://example.com/a%zz")), FEEDREG_ERR_URL_PATH);
    EXPECT_EQ(h.check(withUrl("https://example.com/a#frag")), FEEDREG_ERR_URL_PATH);
    EXPECT_EQ(h.check(withUrl("https://example.com/a b")), FEEDREG_ERR_URL_PATH);
    EXPECT_EQ(h.check(withUrl("https://example.com/@evil.com")), FEEDREG_ERR_URL_PATH);
    EXPECT_EQ(h.check(withUrl("https://example.com/a\\b")), FEEDREG_ERR_URL_PATH);
    EXPECT_EQ(h.check(withUrl("https://example.com/\"x\"")), FEEDREG_ERR_URL_PATH);
}

TEST(Definition, UrlMustBeZeroPaddedAndFitIn255Bytes)
{
    H h;
    R::FeedDefinition d = withUrl("https://example.com/x");
    d.urlTemplate.set(100, 'y');                                   // bytes after the terminator
    EXPECT_EQ(h.check(d), FEEDREG_ERR_URL_PATH);
    std::string longest = "https://example.com/" + std::string(255 - 20, 'a');
    ASSERT_EQ(longest.size(), 255u);
    EXPECT_EQ(h.check(withUrl(longest)), FEEDREG_OK);
    d = withUrl(longest); d.urlTemplate.set(255, 'a');           // 256 bytes, no terminator
    EXPECT_EQ(h.check(d), FEEDREG_ERR_URL_PATH);
    std::string pctAtEnd = "https://example.com/" + std::string(255 - 22, 'a') + "%4";
    EXPECT_EQ(h.check(withUrl(pctAtEnd)), FEEDREG_ERR_URL_PATH);
}

TEST(Definition, PlaceholdersMatchDeclaredParameters)
{
    H h;
    EXPECT_EQ(h.check(withUrl("https://example.com/{0}/{1}", 2)), FEEDREG_OK);
    EXPECT_EQ(h.check(withUrl("https://example.com/{0}/{0}", 1)), FEEDREG_OK);       // reuse is fine
    EXPECT_EQ(h.check(withUrl("https://example.com/{0}", 2)), FEEDREG_ERR_URL_PARAMS);  // {1} never used
    EXPECT_EQ(h.check(withUrl("https://example.com/{1}", 1)), FEEDREG_ERR_URL_PARAMS);  // not declared
    EXPECT_EQ(h.check(withUrl("https://example.com/{4}", 4)), FEEDREG_ERR_URL_PARAMS);
    EXPECT_EQ(h.check(withUrl("https://example.com/{a}", 1)), FEEDREG_ERR_URL_PARAMS);
    EXPECT_EQ(h.check(withUrl("https://example.com/{0", 1)), FEEDREG_ERR_URL_PATH);
    EXPECT_EQ(h.check(withUrl("https://example.com/{00}", 1)), FEEDREG_ERR_URL_PATH);
    EXPECT_EQ(h.check(withUrl("https://example.com/}", 0)), FEEDREG_ERR_URL_PATH);
    EXPECT_EQ(h.check(withUrl("https://{0}.example.com/x", 1)), FEEDREG_ERR_URL_HOST);   // never in the host
    EXPECT_EQ(h.check(withUrl("https://example.com/{0}/{1}/{2}/{3}", 4)), FEEDREG_OK);
}

TEST(Definition, ParamKinds)
{
    H h;
    R::FeedDefinition d = frankfurter();
    d.paramKinds.set(1, 0);                                        // gap: kinds 1,0,2
    EXPECT_EQ(h.check(d), FEEDREG_ERR_PARAM_KINDS);
    d = frankfurter(); d.paramKinds.set(2, FEEDREG_PARAM_KIND_COUNT);
    EXPECT_EQ(h.check(d), FEEDREG_ERR_PARAM_KINDS);
    for (uint8 k = 1; k < FEEDREG_PARAM_KIND_COUNT; k++) { d = frankfurter(); d.paramKinds.set(2, k); EXPECT_EQ(h.check(d), FEEDREG_OK) << (int)k; }
}

TEST(Definition, ValuePath)
{
    H h;
    EXPECT_EQ(h.check(withValuePath("")), FEEDREG_ERR_VALUE_PATH);
    EXPECT_EQ(h.check(withValuePath("rates/USD")), FEEDREG_ERR_VALUE_PATH);
    EXPECT_EQ(h.check(withValuePath("/")), FEEDREG_OK);                    // root value
    EXPECT_EQ(h.check(withValuePath("/a~1b/~0c/0")), FEEDREG_OK);
    EXPECT_EQ(h.check(withValuePath("/a~2")), FEEDREG_ERR_VALUE_PATH);
    EXPECT_EQ(h.check(withValuePath("/a~")), FEEDREG_ERR_VALUE_PATH);
    EXPECT_EQ(h.check(withValuePath("/rates/{3}")), FEEDREG_ERR_VALUE_PATH);  // only 3 params
    EXPECT_EQ(h.check(withValuePath("/a b")), FEEDREG_ERR_VALUE_PATH);
    EXPECT_EQ(h.check(withValuePath("/a?b")), FEEDREG_ERR_VALUE_PATH);
    EXPECT_EQ(h.check(withValuePath("/" + std::string(62, 'a'))), FEEDREG_OK);
    EXPECT_EQ(h.check(withValuePath("/" + std::string(63, 'a'))), FEEDREG_ERR_VALUE_PATH);
    EXPECT_EQ(h.check(withValuePath("/" + std::string(60, 'a') + "{1")), FEEDREG_ERR_VALUE_PATH);
    R::FeedDefinition d = withValuePath("/x"); d.valuePath.set(10, 'z');
    EXPECT_EQ(h.check(d), FEEDREG_ERR_VALUE_PATH);
}

TEST(Definition, TimePolicy)
{
    H h;
    R::FeedDefinition d = frankfurter();
    d.timePolicy = 0; EXPECT_EQ(h.check(d), FEEDREG_ERR_TIME_POLICY);
    d.timePolicy = 3; EXPECT_EQ(h.check(d), FEEDREG_ERR_TIME_POLICY);
    d = frankfurter(); d.minAgeSeconds = 0; EXPECT_EQ(h.check(d), FEEDREG_ERR_TIME_POLICY);
    d.minAgeSeconds = FEEDREG_MIN_AGE_SECONDS - 1; EXPECT_EQ(h.check(d), FEEDREG_ERR_TIME_POLICY);
    d.minAgeSeconds = FEEDREG_MIN_AGE_SECONDS; EXPECT_EQ(h.check(d), FEEDREG_OK);
    d.minAgeSeconds = FEEDREG_MAX_AGE_SECONDS; EXPECT_EQ(h.check(d), FEEDREG_OK);
    d.minAgeSeconds = FEEDREG_MAX_AGE_SECONDS + 1; EXPECT_EQ(h.check(d), FEEDREG_ERR_TIME_POLICY);

    d = frankfurter(); put(d.finalPath, "/final");
    EXPECT_EQ(h.check(d), FEEDREG_ERR_FINAL_PATH);                         // HISTORICAL takes no final flag
    d.timePolicy = FEEDREG_TIME_FINAL; d.minAgeSeconds = 0;
    EXPECT_EQ(h.check(d), FEEDREG_OK);
    d.minAgeSeconds = FEEDREG_MAX_AGE_SECONDS + 1; EXPECT_EQ(h.check(d), FEEDREG_ERR_TIME_POLICY);
    d.minAgeSeconds = 0; put(d.finalPath, "");
    EXPECT_EQ(h.check(d), FEEDREG_ERR_FINAL_PATH);                         // FINAL needs one
    put(d.finalPath, "final");
    EXPECT_EQ(h.check(d), FEEDREG_ERR_FINAL_PATH);
    put(d.finalPath, "/days/{0}/final");
    EXPECT_EQ(h.check(d), FEEDREG_OK);
}

TEST(Definition, ValueTypeFollowsHttpRead)
{
    H h;
    R::FeedDefinition d = frankfurter();
    d.decimals = 0; EXPECT_EQ(h.check(d), FEEDREG_OK);                     // whole numbers
    d.decimals = OI::HttpRead::MAX_DECIMALS; EXPECT_EQ(h.check(d), FEEDREG_OK);
    d.decimals = OI::HttpRead::MAX_DECIMALS + 1; EXPECT_EQ(h.check(d), FEEDREG_ERR_VALUE_TYPE);
    for (uint8 t : {(uint8)OI::HttpRead::ValueType::text, (uint8)OI::HttpRead::ValueType::boolean})
    {
        d.valueType = t; d.decimals = 0; EXPECT_EQ(h.check(d), FEEDREG_OK);
        d.decimals = 2; EXPECT_EQ(h.check(d), FEEDREG_ERR_VALUE_TYPE);
    }
    d.valueType = 0; d.decimals = 0; EXPECT_EQ(h.check(d), FEEDREG_ERR_VALUE_TYPE);
    d.valueType = 4; EXPECT_EQ(h.check(d), FEEDREG_ERR_VALUE_TYPE);
}

TEST(Definition, ReservedByteMustBeZero)
{
    H h;
    R::FeedDefinition d = frankfurter(); d.reserved = 1;
    EXPECT_EQ(h.check(d), FEEDREG_ERR_RESERVED);
}

// ============================================================ proposing

TEST(Propose, StoresFeedHoldsDepositRefundsExcess)
{
    H h;
    sint64 before = bal(ALICE);
    auto o = h.propose(frankfurter(), ALICE, 500, NULL_ID, FEEDREG_DEFAULT_DEPOSIT + 123);
    ASSERT_EQ(o.returnCode, FEEDREG_OK);
    EXPECT_EQ(o.feedId, 1u);
    EXPECT_EQ(bal(ALICE), before - FEEDREG_DEFAULT_DEPOSIT);
    R::Feed f = h.feed(1);
    EXPECT_EQ(f.status, FEEDREG_STATUS_PENDING);
    EXPECT_EQ(f.proposer, ALICE);
    EXPECT_EQ(f.deposit, FEEDREG_DEFAULT_DEPOSIT);
    EXPECT_EQ(f.feePerUse, 500);
    EXPECT_EQ(f.proposedEpoch, chain().epoch);
    EXPECT_EQ(f.definitionHash, o.definitionHash);
    R::FeedDefinition want = frankfurter();
    EXPECT_EQ(0, memcmp(&f.definition, &want, sizeof(want)));
    R::CheckDefinition_input ci{}; ci.definition = frankfurter();
    EXPECT_EQ(h.CheckDefinition(ci).definitionHash, o.definitionHash);
    R::FindByHash_input fi{o.definitionHash};
    EXPECT_EQ(h.FindByHash(fi).feedId, 1u);
    EXPECT_EQ(h.propose(withUrl("https://example.com/x"), BOB).feedId, 2u);
    h.invariant();
}

TEST(Propose, RejectsAndRefundsEverything)
{
    H h;
    sint64 before = bal(ALICE);
    EXPECT_EQ(h.propose(frankfurter(), ALICE, 0, NULL_ID, FEEDREG_DEFAULT_DEPOSIT - 1).returnCode, FEEDREG_ERR_DEPOSIT);
    EXPECT_EQ(h.propose(withUrl("http://example.com/x"), ALICE, 0, NULL_ID, FEEDREG_DEFAULT_DEPOSIT * 3).returnCode, FEEDREG_ERR_URL_SCHEME);
    EXPECT_EQ(h.propose(frankfurter(), ALICE, -1).returnCode, FEEDREG_ERR_FEE);
    EXPECT_EQ(h.propose(frankfurter(), ALICE, FEEDREG_MAX_FEE_PER_USE + 1).returnCode, FEEDREG_ERR_FEE);
    auto o = h.propose(withUrl("http://x.com/a"));
    EXPECT_EQ(o.feedId, 0u);
    EXPECT_EQ(o.definitionHash, NULL_ID);
    EXPECT_EQ(bal(ALICE), before);
    EXPECT_EQ(h.s->feedCount, 0u);
    EXPECT_EQ(h.propose(frankfurter(), ALICE, FEEDREG_MAX_FEE_PER_USE).returnCode, FEEDREG_OK);
    h.invariant();
}

TEST(Propose, DuplicateDefinitionWhileLive)
{
    H h;
    ASSERT_EQ(h.propose(frankfurter()).returnCode, FEEDREG_OK);
    sint64 before = bal(BOB);
    EXPECT_EQ(h.propose(frankfurter(), BOB, 999).returnCode, FEEDREG_ERR_DUPLICATE);   // fee and proposer don't matter
    EXPECT_EQ(bal(BOB), before);
    R::FeedDefinition d = frankfurter(); d.decimals = 7;
    EXPECT_EQ(h.propose(d, BOB).returnCode, FEEDREG_OK);                                 // one byte differs
    h.invariant();
}

TEST(Propose, RegistryFull)
{
    H h;
    h.s->feedCount = FEEDREG_MAX_FEEDS;
    sint64 before = bal(ALICE);
    EXPECT_EQ(h.propose(frankfurter()).returnCode, FEEDREG_ERR_FULL);
    EXPECT_EQ(bal(ALICE), before);
}

// ============================================================ withdrawing

TEST(Withdraw, ProposerOnlyHalfRefundedHalfBurned)
{
    H h;
    auto o = h.propose(frankfurter());
    R::WithdrawFeed_input in{o.feedId};
    EXPECT_EQ(h.WithdrawFeed(in, BOB).returnCode, FEEDREG_ERR_NOT_PROPOSER);
    R::WithdrawFeed_input bad{7};
    EXPECT_EQ(h.WithdrawFeed(bad, ALICE).returnCode, FEEDREG_ERR_NOT_FOUND);
    R::WithdrawFeed_input zero{0};
    EXPECT_EQ(h.WithdrawFeed(zero, ALICE).returnCode, FEEDREG_ERR_NOT_FOUND);
    sint64 before = bal(ALICE);
    auto w = h.WithdrawFeed(in, ALICE, 5);                       // a stray reward is returned too
    EXPECT_EQ(w.returnCode, FEEDREG_OK);
    EXPECT_EQ(w.refunded, FEEDREG_DEFAULT_DEPOSIT / 2);
    EXPECT_EQ(bal(ALICE), before + FEEDREG_DEFAULT_DEPOSIT / 2);
    EXPECT_EQ(chain().burned, FEEDREG_DEFAULT_DEPOSIT / 2);
    EXPECT_EQ(h.status(o.feedId), FEEDREG_STATUS_WITHDRAWN);
    EXPECT_EQ(h.WithdrawFeed(in, ALICE).returnCode, FEEDREG_ERR_WRONG_STATUS);
    R::FindByHash_input fi{o.definitionHash};
    EXPECT_EQ(h.FindByHash(fi).feedId, 0u);
    EXPECT_EQ(h.propose(frankfurter()).feedId, 2u);              // same definition, new id
    h.invariant();
}

TEST(Withdraw, OddDepositRefundsTheLargerHalf)
{
    H h;
    h.s->proposeDeposit = 1001;
    auto o = h.propose(frankfurter(), ALICE, 0, NULL_ID, 1001);
    R::WithdrawFeed_input in{o.feedId};
    EXPECT_EQ(h.WithdrawFeed(in, ALICE).refunded, 501);
    EXPECT_EQ(chain().burned, 500);
    h.invariant();
}

TEST(Withdraw, ApprovedFeedCannotBeWithdrawn)
{
    H h;
    uint64 id1 = h.approved(frankfurter());
    R::WithdrawFeed_input in{id1};
    EXPECT_EQ(h.WithdrawFeed(in, ALICE).returnCode, FEEDREG_ERR_WRONG_STATUS);
}

// ============================================================ governance

TEST(Governance, ApprovalRefundsDeposit)
{
    H h;
    auto o = h.propose(frankfurter());
    sint64 before = bal(ALICE);
    h.pass(FEEDREG_VAR_APPROVE_FEED, (sint64)o.feedId);
    EXPECT_EQ(h.status(o.feedId), FEEDREG_STATUS_PENDING);        // only at END_EPOCH
    uint16 epoch = chain().epoch;
    h.endEpoch();
    R::Feed f = h.feed(o.feedId);
    EXPECT_EQ(f.status, FEEDREG_STATUS_APPROVED);
    EXPECT_EQ(f.decidedEpoch, epoch);
    EXPECT_EQ(f.deposit, 0);
    EXPECT_EQ(bal(ALICE), before + FEEDREG_DEFAULT_DEPOSIT);
    EXPECT_EQ(chain().burned, FEEDREG_DEFAULT_PROPOSAL_FEE);     // only the shareholder's proposal fee
    R::FindByHash_input fi{o.definitionHash};
    EXPECT_EQ(h.FindByHash(fi).feedId, o.feedId);                // still live: no duplicate
    EXPECT_EQ(h.propose(frankfurter(), BOB).returnCode, FEEDREG_ERR_DUPLICATE);
    h.invariant();
}

TEST(Governance, RejectedVoteLeavesFeedPendingThenItExpires)
{
    H h;
    auto o = h.propose(frankfurter());
    uint16 idx = h.openVote(FEEDREG_VAR_APPROVE_FEED, (sint64)o.feedId);
    h.vote(SH1, idx, 0);
    h.vote(SH2, idx, 1);                                           // 400 no, 276 yes
    h.endEpoch();                                                  // epoch E
    EXPECT_EQ(h.status(o.feedId), FEEDREG_STATUS_PENDING);
    h.endEpoch();                                                  // E+1
    h.endEpoch();                                                  // E+2
    EXPECT_EQ(h.status(o.feedId), FEEDREG_STATUS_PENDING);
    sint64 burnedBefore = chain().burned;
    h.endEpoch();                                                  // E+3: last voting epoch over
    EXPECT_EQ(h.status(o.feedId), FEEDREG_STATUS_EXPIRED);
    EXPECT_EQ(chain().burned - burnedBefore, FEEDREG_DEFAULT_DEPOSIT);
    R::FindByHash_input fi{o.definitionHash};
    EXPECT_EQ(h.FindByHash(fi).feedId, 0u);
    h.invariant();
}

TEST(Governance, NoQuorumIsNotApproval)
{
    H h;
    auto o = h.propose(frankfurter());
    uint16 idx = h.openVote(FEEDREG_VAR_APPROVE_FEED, (sint64)o.feedId);
    h.vote(SH1, idx, 1);                                           // 400 of 676 < 451
    h.endEpoch();
    EXPECT_EQ(h.status(o.feedId), FEEDREG_STATUS_PENDING);
}

TEST(Governance, ApprovalInTheLastVotingEpochWinsOverExpiry)
{
    H h;
    auto o = h.propose(frankfurter());
    h.endEpoch(); h.endEpoch(); h.endEpoch();
    h.pass(FEEDREG_VAR_APPROVE_FEED, (sint64)o.feedId);
    h.endEpoch();
    EXPECT_EQ(h.status(o.feedId), FEEDREG_STATUS_APPROVED);
    h.invariant();
}

TEST(Governance, VoteCannotBeOpenedOnAnExpiredFeed)
{
    H h;
    auto o = h.propose(frankfurter());
    for (int i = 0; i < FEEDREG_PENDING_EPOCHS; i++) h.endEpoch();
    EXPECT_EQ(h.status(o.feedId), FEEDREG_STATUS_EXPIRED);
    EXPECT_EQ(h.openVote(FEEDREG_VAR_APPROVE_FEED, (sint64)o.feedId), INVALID_PROPOSAL_INDEX);
}

TEST(Governance, WithdrawnDuringVoteIsNotApprovedAndNotRefundedTwice)
{
    H h;
    auto o = h.propose(frankfurter());
    h.pass(FEEDREG_VAR_APPROVE_FEED, (sint64)o.feedId);
    R::WithdrawFeed_input in{o.feedId};
    ASSERT_EQ(h.WithdrawFeed(in, ALICE).returnCode, FEEDREG_OK);
    sint64 before = bal(ALICE);
    h.endEpoch();
    EXPECT_EQ(h.status(o.feedId), FEEDREG_STATUS_WITHDRAWN);
    EXPECT_EQ(bal(ALICE), before);
    h.invariant();
}

TEST(Governance, InvalidProposalsAreRefused)
{
    H h;
    auto p = h.propose(frankfurter());
    uint64 a = h.approved(withUrl("https://example.com/a"));
    sint64 before = bal(SH1);
    EXPECT_EQ(h.openVote(FEEDREG_VAR_APPROVE_FEED, 0), INVALID_PROPOSAL_INDEX);
    EXPECT_EQ(h.openVote(FEEDREG_VAR_APPROVE_FEED, 99), INVALID_PROPOSAL_INDEX);
    EXPECT_EQ(h.openVote(FEEDREG_VAR_APPROVE_FEED, (sint64)a), INVALID_PROPOSAL_INDEX);           // already approved
    EXPECT_EQ(h.openVote(FEEDREG_VAR_RETIRE_FEED, (sint64)p.feedId), INVALID_PROPOSAL_INDEX);     // still pending
    EXPECT_EQ(h.openVote(FEEDREG_VAR_DEPOSIT, FEEDREG_MIN_DEPOSIT - 1), INVALID_PROPOSAL_INDEX);
    EXPECT_EQ(h.openVote(FEEDREG_VAR_DEPOSIT, FEEDREG_MAX_DEPOSIT + 1), INVALID_PROPOSAL_INDEX);
    EXPECT_EQ(h.openVote(FEEDREG_VAR_PROPOSER_SHARE_BPS, FEEDREG_MAX_PROPOSER_SHARE_BPS + 1), INVALID_PROPOSAL_INDEX);
    EXPECT_EQ(h.openVote(FEEDREG_VAR_PROPOSAL_FEE, FEEDREG_MAX_PROPOSAL_FEE + 1), INVALID_PROPOSAL_INDEX);
    EXPECT_EQ(h.openVote(FEEDREG_VARIABLE_COUNT, 1), INVALID_PROPOSAL_INDEX);
    EXPECT_EQ(h.openVote(FEEDREG_VAR_DEPOSIT, -5), INVALID_PROPOSAL_INDEX);
    EXPECT_EQ(h.openVote(FEEDREG_VAR_DEPOSIT, 20000000, SH1, FEEDREG_DEFAULT_PROPOSAL_FEE - 1), INVALID_PROPOSAL_INDEX);  // fee short
    EXPECT_EQ(bal(SH1), before);
    sint64 c = bal(CAROL);
    EXPECT_EQ(h.openVote(FEEDREG_VAR_DEPOSIT, 20000000, CAROL), INVALID_PROPOSAL_INDEX);             // not a shareholder
    EXPECT_EQ(bal(CAROL), c);
    R::SetShareholderProposal_input yn; memset((void*)&yn, 0, sizeof(yn));
    yn.epoch = chain().epoch; yn.type = ProposalTypes::YesNo;
    EXPECT_EQ(h.SetShareholderProposal(yn, SH1, FEEDREG_DEFAULT_PROPOSAL_FEE), INVALID_PROPOSAL_INDEX);  // only variable proposals
    EXPECT_EQ(bal(SH1), before);
    EXPECT_EQ(chain().burned, FEEDREG_DEFAULT_PROPOSAL_FEE);     // just the one from approved()
    h.invariant();
}

TEST(Governance, ProposalFeeExcessIsRefunded)
{
    H h;
    sint64 before = bal(SH1);
    EXPECT_NE(h.openVote(FEEDREG_VAR_DEPOSIT, 20000000, SH1, FEEDREG_DEFAULT_PROPOSAL_FEE + 77), INVALID_PROPOSAL_INDEX);
    EXPECT_EQ(bal(SH1), before - FEEDREG_DEFAULT_PROPOSAL_FEE);
    EXPECT_EQ(chain().burned, FEEDREG_DEFAULT_PROPOSAL_FEE);
}

TEST(Governance, RetireFreesTheDefinition)
{
    H h;
    uint64 id1 = h.approved(frankfurter());
    h.pass(FEEDREG_VAR_RETIRE_FEED, (sint64)id1);
    h.endEpoch();
    EXPECT_EQ(h.status(id1), FEEDREG_STATUS_RETIRED);
    EXPECT_EQ(h.request(id1, DateAndTime(2026, 11, 20), {"2026-11-20", "EUR", "USD"}).returnCode, FEEDREG_ERR_WRONG_STATUS);
    EXPECT_EQ(h.openVote(FEEDREG_VAR_RETIRE_FEED, (sint64)id1), INVALID_PROPOSAL_INDEX);
    EXPECT_EQ(h.openVote(FEEDREG_VAR_APPROVE_FEED, (sint64)id1), INVALID_PROPOSAL_INDEX);       // no way back
    EXPECT_EQ(h.propose(frankfurter()).feedId, 2u);              // can be proposed afresh
    h.invariant();
}

TEST(Governance, ParametersChangeByVote)
{
    H h;
    h.pass(FEEDREG_VAR_DEPOSIT, 20000000);
    h.endEpoch();
    h.pass(FEEDREG_VAR_PROPOSER_SHARE_BPS, 0);
    h.endEpoch();
    h.pass(FEEDREG_VAR_PROPOSAL_FEE, 0);
    h.endEpoch();
    R::GetInfo_input gi{};
    auto info = h.GetInfo(gi);
    EXPECT_EQ(info.proposeDeposit, 20000000);
    EXPECT_EQ(info.proposerShareBps, 0);
    EXPECT_EQ(info.proposalFee, 0);
    auto o = h.propose(frankfurter(), ALICE, 0, NULL_ID, 20000000);
    EXPECT_EQ(o.returnCode, FEEDREG_OK);
    EXPECT_EQ(h.feed(o.feedId).deposit, 20000000);
    EXPECT_NE(h.openVote(FEEDREG_VAR_APPROVE_FEED, (sint64)o.feedId, SH1, 0), INVALID_PROPOSAL_INDEX);
    h.invariant();
}

TEST(Governance, DepositChangeDoesNotTouchHeldDeposits)
{
    H h;
    auto o = h.propose(frankfurter());
    h.pass(FEEDREG_VAR_DEPOSIT, 20000000);
    h.endEpoch();
    h.pass(FEEDREG_VAR_APPROVE_FEED, (sint64)o.feedId);
    sint64 before = bal(ALICE);
    h.endEpoch();
    EXPECT_EQ(bal(ALICE), before + FEEDREG_DEFAULT_DEPOSIT);     // the deposit actually paid
    h.invariant();
}

// ============================================================ requesting values

static const DateAndTime NOV20 = DateAndTime(2026, 11, 20);
static const std::vector<std::string> EURUSD = {"2026-11-20", "EUR", "USD"};

TEST(Request, SendsHttpReadQueryAndRecordsIt)
{
    H h;
    uint64 f = h.approved(frankfurter(), 1000);
    R::GetFeed_input gi{f};
    EXPECT_EQ(h.GetFeed(gi).requestCost, 200 + 1000);
    sint64 consumer = bal(CONSUMER);
    auto o = h.request(f, NOV20, EURUSD, CONSUMER, 1200 + 99);           // 99 too much
    ASSERT_EQ(o.returnCode, FEEDREG_OK);
    EXPECT_EQ(o.requestId, 1u);
    EXPECT_EQ(o.charged, 1200);
    EXPECT_EQ(bal(CONSUMER), consumer - 1200);
    EXPECT_EQ(chain().oracleFeesPaid, 200);
    EXPECT_EQ(h.s->heldFees, 1000);

    ASSERT_EQ(chain().queries.size(), 1u);
    const auto& q = chain().queries.back();
    EXPECT_EQ(q.query.source, OI::HttpRead::getFeedRegistrySourceId());
    EXPECT_EQ(q.query.endpoint, o.requestId);
    EXPECT_EQ(q.query.timestamp, NOV20);
    EXPECT_EQ(q.query.valueType, OI::HttpRead::ValueType::decimal);
    EXPECT_EQ(q.query.decimals, 6u);
    EXPECT_EQ(q.query.params.get(32), 'E');
    EXPECT_EQ(q.query.params.get(64 + 2), 'D');
    for (int i = 0; i < 64; i++) EXPECT_EQ(q.query.field.get(i), 0);
    EXPECT_EQ(q.timeoutMs, FEEDREG_ORACLE_TIMEOUT_MS);

    R::Request r = h.req(o.requestId);
    EXPECT_EQ(r.status, FEEDREG_REQUEST_PENDING);
    EXPECT_EQ(r.feedId, f);
    EXPECT_EQ(r.requester, CONSUMER);
    EXPECT_EQ(r.definitionHash, h.feed(f).definitionHash);
    EXPECT_EQ(r.oracleQueryId, q.queryId);               // what the oracle machine matches against
    EXPECT_EQ(r.timestamp, NOV20);
    EXPECT_EQ(0, memcmp(&r.params, &q.query.params, sizeof(r.params)));
    EXPECT_EQ(r.feeHeld, 1000);
    EXPECT_EQ(h.request(f, NOV20, EURUSD).requestId, 2u);
    h.invariant();
}

TEST(Request, AnswerPaysProposerAndShareholders)
{
    H h;
    uint64 f = h.approved(frankfurter(), 1000);
    auto o = h.request(f, NOV20, EURUSD);
    sint64 alice = bal(ALICE);
    h.notify(h.lastQueryId(), ORACLE_QUERY_STATUS_SUCCESS, 1053200);
    R::Request r = h.req(o.requestId);
    EXPECT_EQ(r.status, FEEDREG_REQUEST_ANSWERED);
    EXPECT_EQ(r.reply.code, OI::HttpRead::RESULT_SUCCESS);
    EXPECT_EQ(r.reply.intValue, 1053200);                // 1.0532 USD per EUR, 6 decimals
    EXPECT_EQ(r.feeHeld, 0);
    EXPECT_EQ(r.answerTick, chain().tick);
    EXPECT_EQ(bal(ALICE), alice + 200);                  // 20% of the feed fee
    EXPECT_EQ(h.s->dividendPool, 800);
    EXPECT_EQ(h.s->heldFees, 0);
    EXPECT_EQ(h.feed(f).requestCount, 1u);
    h.invariant();
    h.notify(h.lastQueryId(), ORACLE_QUERY_STATUS_SUCCESS, 1);    // a repeat changes nothing
    EXPECT_EQ(h.req(o.requestId).reply.intValue, 1053200);
    EXPECT_EQ(bal(ALICE), alice + 200);
    h.endEpoch();
    EXPECT_EQ(chain().dividends, 676);                   // 1 QU per share; 124 stay in the pool
    EXPECT_EQ(h.s->dividendPool, 124);
    h.invariant();
}

TEST(Request, SourceSaysNoDataStillCountsAsAnswered)
{
    H h;
    uint64 f = h.approved(frankfurter(), 1000);
    auto o = h.request(f, NOV20, EURUSD);
    h.notify(h.lastQueryId(), ORACLE_QUERY_STATUS_SUCCESS, 0, OI::HttpRead::RESULT_NO_DATA);
    R::Request r = h.req(o.requestId);
    EXPECT_EQ(r.status, FEEDREG_REQUEST_ANSWERED);
    EXPECT_EQ(r.reply.code, OI::HttpRead::RESULT_NO_DATA);
    EXPECT_EQ(h.s->dividendPool, 800);                   // the source was read; the fee is earned
    h.invariant();
}

TEST(Request, NoAnswerRefundsTheFeedFeeOnly)
{
    for (uint8 status : {ORACLE_QUERY_STATUS_TIMEOUT, ORACLE_QUERY_STATUS_UNRESOLVABLE})
    {
        H h;
        uint64 f = h.approved(frankfurter(), 1000);
        sint64 consumer = bal(CONSUMER), alice = bal(ALICE);
        auto o = h.request(f, NOV20, EURUSD);
        h.notify(h.lastQueryId(), status);
        R::Request r = h.req(o.requestId);
        EXPECT_EQ(r.status, FEEDREG_REQUEST_NOT_ANSWERED);
        EXPECT_EQ(r.reply.code, 0u);
        EXPECT_EQ(bal(CONSUMER), consumer - 200);        // the oracle fee is gone, the feed fee is back
        EXPECT_EQ(bal(ALICE), alice);
        EXPECT_EQ(h.s->dividendPool, 0);
        EXPECT_EQ(h.feed(f).requestCount, 0u);
        h.invariant();
    }
}

TEST(Request, UnknownNotificationIsIgnored)
{
    H h;
    uint64 f = h.approved(frankfurter(), 1000);
    auto o = h.request(f, NOV20, EURUSD);
    h.notify(h.lastQueryId() + 1, ORACLE_QUERY_STATUS_SUCCESS, 5);
    EXPECT_EQ(h.req(o.requestId).status, FEEDREG_REQUEST_PENDING);
    h.invariant();
}

TEST(Request, FreeFeedCostsOnlyTheOracleFee)
{
    H h;
    uint64 f = h.approved(frankfurter());
    auto o = h.request(f, NOV20, EURUSD, CONSUMER, 200);
    EXPECT_EQ(o.returnCode, FEEDREG_OK);
    EXPECT_EQ(o.charged, 200);
    h.notify(h.lastQueryId(), ORACLE_QUERY_STATUS_SUCCESS, 7);
    EXPECT_EQ(h.s->dividendPool, 0);
    h.invariant();
}

TEST(Request, RefusalsRefundEverything)
{
    H h;
    uint64 f = h.approved(frankfurter(), 1000);
    auto pending = h.propose(withUrl("https://example.com/x"));
    sint64 before = bal(CONSUMER);
    EXPECT_EQ(h.request(99, NOV20, EURUSD, CONSUMER, 5000).returnCode, FEEDREG_ERR_NOT_FOUND);
    EXPECT_EQ(h.request(0, NOV20, EURUSD, CONSUMER, 5000).returnCode, FEEDREG_ERR_NOT_FOUND);
    EXPECT_EQ(h.request(pending.feedId, NOV20, {}, CONSUMER, 5000).returnCode, FEEDREG_ERR_WRONG_STATUS);
    EXPECT_EQ(h.request(f, DateAndTime(), EURUSD, CONSUMER, 5000).returnCode, FEEDREG_ERR_TIMESTAMP);
    EXPECT_EQ(h.request(f, NOV20, EURUSD, CONSUMER, 1199).returnCode, FEEDREG_ERR_PAYMENT);
    chain().oracleRejects = true;
    EXPECT_EQ(h.request(f, NOV20, EURUSD, CONSUMER, 5000).returnCode, FEEDREG_ERR_ORACLE_REFUSED);
    chain().oracleRejects = false;
    EXPECT_EQ(bal(CONSUMER), before);
    EXPECT_EQ(chain().queries.size(), 0u);
    EXPECT_EQ(h.s->nextRequestId, 1u);
    h.invariant();
}

TEST(Request, MinimumAgeOfTheRequestedMoment)
{
    H h;
    uint64 f = h.approved(frankfurter());                // minAgeSeconds = 86400
    DateAndTime edge = chain().now; edge.add(0, 0, -1, 0, 0, 0);
    DateAndTime late = edge; late.add(0, 0, 0, 0, 0, 1);
    EXPECT_EQ(h.request(f, late, EURUSD).returnCode, FEEDREG_ERR_TOO_RECENT);
    EXPECT_EQ(h.request(f, chain().now, EURUSD).returnCode, FEEDREG_ERR_TOO_RECENT);
    EXPECT_EQ(h.request(f, edge, EURUSD).returnCode, FEEDREG_OK);
    DateAndTime future = chain().now; future.add(0, 0, 3, 0, 0, 0);
    EXPECT_EQ(h.request(f, future, EURUSD).returnCode, FEEDREG_ERR_TOO_RECENT);
    h.invariant();
}

TEST(Request, FinalFeedWithoutMinimumAgeAcceptsNow)
{
    H h;
    R::FeedDefinition d = frankfurter();
    d.timePolicy = FEEDREG_TIME_FINAL; d.minAgeSeconds = 0; put(d.finalPath, "/final");
    uint64 f = h.approved(d);
    EXPECT_EQ(h.request(f, chain().now, EURUSD).returnCode, FEEDREG_OK);
    DateAndTime soon = chain().now; soon.add(0, 0, 0, 0, 0, 1);
    EXPECT_EQ(h.request(f, soon, EURUSD).returnCode, FEEDREG_ERR_TOO_RECENT);
}

TEST(Request, RestrictedFeedServesOnlyItsContract)
{
    H h;
    uint64 f = h.approved(frankfurter(), 0, CONSUMER);
    sint64 before = bal(CONSUMER2);
    EXPECT_EQ(h.request(f, NOV20, EURUSD, CONSUMER2).returnCode, FEEDREG_ERR_RESTRICTED);
    EXPECT_EQ(bal(CONSUMER2), before);
    auto o = h.request(f, NOV20, EURUSD, CONSUMER);
    EXPECT_EQ(o.returnCode, FEEDREG_OK);
    EXPECT_EQ(h.req(o.requestId).requester, CONSUMER);
    h.invariant();
}

TEST(Request, RetiredWhilePendingIsStillAnsweredAndPaid)
{
    H h;
    uint64 f = h.approved(frankfurter(), 1000);
    auto o = h.request(f, NOV20, EURUSD);
    h.pass(FEEDREG_VAR_RETIRE_FEED, (sint64)f);
    h.endEpoch();
    ASSERT_EQ(h.status(f), FEEDREG_STATUS_RETIRED);
    sint64 alice = bal(ALICE);
    h.notify(h.req(o.requestId).oracleQueryId, ORACLE_QUERY_STATUS_SUCCESS, 9);
    EXPECT_EQ(h.req(o.requestId).status, FEEDREG_REQUEST_ANSWERED);
    EXPECT_EQ(bal(ALICE), alice + 200);
    h.invariant();
}

TEST(Request, RingOfRecentRequests)
{
    H h;
    uint64 f = h.approved(frankfurter());
    std::vector<sint64> qids;
    for (uint64 i = 0; i < FEEDREG_MAX_REQUESTS; i++)
    {
        ASSERT_EQ(h.request(f, NOV20, EURUSD).returnCode, FEEDREG_OK) << i;
        qids.push_back(h.lastQueryId());
    }
    sint64 before = bal(CONSUMER);
    EXPECT_EQ(h.request(f, NOV20, EURUSD, CONSUMER, 200).returnCode, FEEDREG_ERR_BUSY);   // 1025 would reuse request 1's slot
    EXPECT_EQ(bal(CONSUMER), before);
    h.notify(qids[1], ORACLE_QUERY_STATUS_SUCCESS, 1);                                  // request 2 answered: not the slot needed
    EXPECT_EQ(h.request(f, NOV20, EURUSD, CONSUMER, 200).returnCode, FEEDREG_ERR_BUSY);
    h.notify(qids[0], ORACLE_QUERY_STATUS_SUCCESS, 1);                                  // request 1 answered
    auto o = h.request(f, NOV20, EURUSD);
    EXPECT_EQ(o.returnCode, FEEDREG_OK);
    EXPECT_EQ(o.requestId, FEEDREG_MAX_REQUESTS + 1);
    R::GetRequest_input old{1};
    EXPECT_EQ(h.GetRequest(old).returnCode, FEEDREG_ERR_NOT_FOUND);                    // overwritten
    R::GetRequest_input still{FEEDREG_MAX_REQUESTS};
    EXPECT_EQ(h.GetRequest(still).returnCode, FEEDREG_OK);
    EXPECT_EQ(h.req(FEEDREG_MAX_REQUESTS + 1).status, FEEDREG_REQUEST_PENDING);
    R::GetRequest_input zero{0}, future{5000};
    EXPECT_EQ(h.GetRequest(zero).returnCode, FEEDREG_ERR_NOT_FOUND);
    EXPECT_EQ(h.GetRequest(future).returnCode, FEEDREG_ERR_NOT_FOUND);
    h.invariant();
}

// ---- parameters, checked against the feed's kinds

static uint32 paramResult(uint8 kind, const std::string& value)
{
    H h;
    R::FeedDefinition d = withUrl("https://example.com/{0}", 1);
    d.paramKinds.set(0, kind);
    uint64 f = h.approved(d);
    return h.request(f, NOV20, {value}).returnCode;
}

TEST(Params, IsoDate)
{
    for (auto v : {"2026-11-20", "2024-02-29", "0001-01-01", "2026-12-31"}) EXPECT_EQ(paramResult(FEEDREG_PARAM_ISO_DATE, v), FEEDREG_OK) << v;
    for (auto v : {"2023-02-29", "2026-13-01", "2026-00-10", "2026-04-31", "2026-1-01", "2026/01/01", "2026-01-011", "20260101", "2026-0a-01", "2026-0:-01", "20:6-01-01", "2026-01-0:", ""})
        EXPECT_EQ(paramResult(FEEDREG_PARAM_ISO_DATE, v), FEEDREG_ERR_PARAMS) << v;
}

TEST(Params, Currency)
{
    EXPECT_EQ(paramResult(FEEDREG_PARAM_CURRENCY, "USD"), FEEDREG_OK);
    for (auto v : {"usd", "US", "USDT", "U$D", "1SD"}) EXPECT_EQ(paramResult(FEEDREG_PARAM_CURRENCY, v), FEEDREG_ERR_PARAMS) << v;
}

TEST(Params, Integer)
{
    for (auto v : {"0", "-12", "123456789012345678", "-123456789012345678"}) EXPECT_EQ(paramResult(FEEDREG_PARAM_INTEGER, v), FEEDREG_OK) << v;
    for (auto v : {"1.5", "-", "1234567890123456789", "+1", "1a", "1-", "--1"}) EXPECT_EQ(paramResult(FEEDREG_PARAM_INTEGER, v), FEEDREG_ERR_PARAMS) << v;
}

TEST(Params, Decimal)
{
    for (auto v : {"1.5", "-0.25", "12", "0.123456789012345678"}) EXPECT_EQ(paramResult(FEEDREG_PARAM_DECIMAL, v), FEEDREG_OK) << v;
    for (auto v : {"1.", ".5", "1.2.3", "0.1234567890123456789", "-.5", "1e5"}) EXPECT_EQ(paramResult(FEEDREG_PARAM_DECIMAL, v), FEEDREG_ERR_PARAMS) << v;
}

TEST(Params, Slug)
{
    for (std::string v : {std::string("abc-1"), std::string("a"), std::string("0"), std::string(32, 'z')}) EXPECT_EQ(paramResult(FEEDREG_PARAM_SLUG, v), FEEDREG_OK) << v;
    for (auto v : {"ABC", "a_b", "a b", "a.b", ""}) EXPECT_EQ(paramResult(FEEDREG_PARAM_SLUG, v), FEEDREG_ERR_PARAMS) << v;
}

TEST(Params, Coordinate)
{
    for (auto v : {"180", "-180.0000", "53.3811", "-1.4701", "0"}) EXPECT_EQ(paramResult(FEEDREG_PARAM_COORDINATE, v), FEEDREG_OK) << v;
    for (auto v : {"180.0001", "181", "12.34567", "1234", "0045", "-", "5."}) EXPECT_EQ(paramResult(FEEDREG_PARAM_COORDINATE, v), FEEDREG_ERR_PARAMS) << v;
}

TEST(Params, PaddingAndUnusedSlots)
{
    H h;
    uint64 f = h.approved(frankfurter());
    EXPECT_EQ(h.request(f, NOV20, {"2026-11-20", "EUR", "USD", "X"}).returnCode, FEEDREG_ERR_PARAMS);   // 4th not declared
    EXPECT_EQ(h.request(f, NOV20, {"2026-11-20", "EUR"}).returnCode, FEEDREG_ERR_PARAMS);             // 3rd missing
    EXPECT_EQ(h.request(f, NOV20, {"2026-11-20", std::string("EUR\0X", 5), "USD"}).returnCode, FEEDREG_ERR_PARAMS);  // bytes after the end
    R::RequestValue_input in; memset((void*)&in, 0, sizeof(in));
    in.feedId = f; in.timestamp = NOV20;
    for (int j = 0; j < 3; j++) for (int i = 0; i < (int)EURUSD[j].size(); i++) in.params.set(j * 32 + i, EURUSD[j][i]);
    in.params.set(127, 1);                                                                          // last byte of slot 4
    EXPECT_EQ(h.RequestValue(in, CONSUMER, 200).returnCode, FEEDREG_ERR_PARAMS);
    in.params.set(127, 0);
    EXPECT_EQ(h.RequestValue(in, CONSUMER, 200).returnCode, FEEDREG_OK);
    h.invariant();
}

// ============================================================ listing

TEST(List, PagesAndFilters)
{
    H h;
    for (int i = 0; i < 20; i++) ASSERT_EQ(h.propose(withUrl("https://example.com/f" + std::to_string(i)), ALICE, i).returnCode, FEEDREG_OK);
    h.pass(FEEDREG_VAR_APPROVE_FEED, 5);
    h.endEpoch();
    R::ListFeeds_input in{0, 0};
    auto p1 = h.ListFeeds(in);
    EXPECT_EQ(p1.count, FEEDREG_LIST_PAGE);
    EXPECT_EQ(p1.nextFeedId, 17u);
    EXPECT_EQ(p1.feeds.get(0).feedId, 1u);
    EXPECT_EQ(p1.feeds.get(3).feePerUse, 3);
    in.startFeedId = p1.nextFeedId;
    auto p2 = h.ListFeeds(in);
    EXPECT_EQ(p2.count, 4u);
    EXPECT_EQ(p2.nextFeedId, 0u);
    EXPECT_EQ(p2.feeds.get(3).feedId, 20u);
    R::ListFeeds_input ap{1, FEEDREG_STATUS_APPROVED};
    auto a = h.ListFeeds(ap);
    EXPECT_EQ(a.count, 1u);
    EXPECT_EQ(a.feeds.get(0).feedId, 5u);
    EXPECT_EQ(a.feeds.get(0).status, FEEDREG_STATUS_APPROVED);
    EXPECT_EQ(a.feeds.get(0).definitionHash, h.feed(5).definitionHash);
    R::ListFeeds_input none{21, 0};
    EXPECT_EQ(h.ListFeeds(none).count, 0u);
}

TEST(List, SummaryFields)
{
    H h;
    h.approved(frankfurter(), 0, CONSUMER);
    R::ListFeeds_input in{1, 0};
    auto s = h.ListFeeds(in).feeds.get(0);
    EXPECT_EQ(s.restricted, 1);
    EXPECT_EQ(s.timePolicy, FEEDREG_TIME_HISTORICAL);
    EXPECT_EQ(s.valueType, OI::HttpRead::ValueType::decimal);
    EXPECT_EQ(s.decimals, 6);
}

// ============================================================ a longer run

TEST(Scenario, MoneyIsAlwaysAccountedFor)
{
    H h;
    uint64 seed = 12345;
    auto rnd = [&](uint64 n) { seed = seed * 6364136223846793005ULL + 1442695040888963407ULL; return (seed >> 33) % n; };
    std::vector<sint64> open;
    for (int step = 0; step < 3000; step++)
    {
        switch (rnd(7))
        {
        case 0: h.propose(withUrl("https://example.com/s" + std::to_string(rnd(40))), rnd(2) ? ALICE : BOB, (sint64)rnd(3) * 1000, rnd(5) ? NULL_ID : CONSUMER, FEEDREG_DEFAULT_DEPOSIT + (sint64)rnd(3)); break;
        case 1: { R::WithdrawFeed_input in{rnd(h.s->feedCount + 2)}; h.WithdrawFeed(in, rnd(2) ? ALICE : BOB); break; }
        case 2: if (h.s->feedCount)
            {
                uint64 f = 1 + rnd(h.s->feedCount);
                for (uint64 k = 0; k < h.s->feedCount && h.s->feeds.get(f - 1).status != FEEDREG_STATUS_PENDING; k++) f = f % h.s->feedCount + 1;
                uint16 i = h.openVote(rnd(4) ? FEEDREG_VAR_APPROVE_FEED : FEEDREG_VAR_RETIRE_FEED, (sint64)(rnd(4) ? f : 1 + rnd(h.s->feedCount)), rnd(2) ? SH1 : SH2);
                if (i != INVALID_PROPOSAL_INDEX) { h.vote(SH1, i, 1); h.vote(SH2, i, 1); }
            }
            break;
        case 3: if (h.s->feedCount) { auto o = h.request(1 + rnd(h.s->feedCount), NOV20, {}, rnd(2) ? CONSUMER : CONSUMER2, 150 + (sint64)rnd(2500)); if (o.returnCode == FEEDREG_OK) open.push_back(h.lastQueryId()); } break;
        case 4: if (!open.empty()) { size_t k = rnd(open.size()); static const uint8 st[] = {ORACLE_QUERY_STATUS_SUCCESS, ORACLE_QUERY_STATUS_TIMEOUT, ORACLE_QUERY_STATUS_UNRESOLVABLE}; h.notify(open[k], st[rnd(3)], 5); open.erase(open.begin() + k); } break;
        case 5: if (rnd(3) == 0) h.endEpoch(); break;
        default: break;
        }
        h.invariant();
        if (::testing::Test::HasFailure()) { ADD_FAILURE() << "step " << step; return; }
    }
    EXPECT_GT(h.s->feedCount, 10u);
    EXPECT_GT(h.s->nextRequestId, 50u);
    EXPECT_GT(chain().dividends, 0);
}
