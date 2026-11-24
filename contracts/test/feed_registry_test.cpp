// Native tests for FeedRegistry.h against mock_qpi.h.
#include <gtest/gtest.h>
#include <string>
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
    d.valueType = FEEDREG_VALUE_DECIMAL;
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
    P(ProposeFeed) P(WithdrawFeed) P(BuyAccess) P(SetShareholderProposal) P(SetShareholderVotes)
    F(GetFeed) F(ListFeeds) F(GetAccess) F(FindByHash) F(CheckDefinition) F(GetInfo) F(GetShareholderProposal)

    uint32 check(const R::FeedDefinition& d) { R::CheckDefinition_input in{}; in.definition = d; return CheckDefinition(in).returnCode; }

    R::ProposeFeed_output propose(const R::FeedDefinition& d, const id& who = ALICE, sint64 fee = 0, const id& restrictedTo = NULL_ID, sint64 pay = FEEDREG_DEFAULT_DEPOSIT)
    {
        R::ProposeFeed_input in; memset(&in, 0, sizeof(in));
        in.definition = d; in.feePerEpoch = fee; in.restrictedTo = restrictedTo;
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
    R::BuyAccess_output buy(uint64 feedId, uint16 epochs, const id& who = CONSUMER, sint64 pay = -1)
    {
        R::BuyAccess_input in{feedId, epochs};
        if (pay < 0) pay = feed(feedId).feePerEpoch * epochs;
        return BuyAccess(in, who, pay);
    }
    R::GetAccess_output access(uint64 feedId, const id& user) { R::GetAccess_input in{}; in.feedId = feedId; in.user = user; return GetAccess(in); }

    // the contract never holds QU it cannot account for
    void invariant()
    {
        EXPECT_EQ(bal(MOCK_CONTRACT_ID), s->heldDeposits + s->dividendPool);
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

TEST(Definition, ValueType)
{
    H h;
    R::FeedDefinition d = frankfurter();
    d.decimals = 0; EXPECT_EQ(h.check(d), FEEDREG_ERR_VALUE_TYPE);
    d.decimals = FEEDREG_MAX_DECIMALS; EXPECT_EQ(h.check(d), FEEDREG_OK);
    d.decimals = FEEDREG_MAX_DECIMALS + 1; EXPECT_EQ(h.check(d), FEEDREG_ERR_VALUE_TYPE);
    for (uint8 t : {FEEDREG_VALUE_INTEGER, FEEDREG_VALUE_TEXT, FEEDREG_VALUE_BOOL})
    {
        d.valueType = t; d.decimals = 0; EXPECT_EQ(h.check(d), FEEDREG_OK);
        d.decimals = 2; EXPECT_EQ(h.check(d), FEEDREG_ERR_VALUE_TYPE);
    }
    d.valueType = 0; d.decimals = 0; EXPECT_EQ(h.check(d), FEEDREG_ERR_VALUE_TYPE);
    d.valueType = 5; EXPECT_EQ(h.check(d), FEEDREG_ERR_VALUE_TYPE);
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
    EXPECT_EQ(f.feePerEpoch, 500);
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
    EXPECT_EQ(h.propose(frankfurter(), ALICE, FEEDREG_MAX_FEE_PER_EPOCH + 1).returnCode, FEEDREG_ERR_FEE);
    auto o = h.propose(withUrl("http://x.com/a"));
    EXPECT_EQ(o.feedId, 0u);
    EXPECT_EQ(o.definitionHash, NULL_ID);
    EXPECT_EQ(bal(ALICE), before);
    EXPECT_EQ(h.s->feedCount, 0u);
    EXPECT_EQ(h.propose(frankfurter(), ALICE, FEEDREG_MAX_FEE_PER_EPOCH).returnCode, FEEDREG_OK);
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
    EXPECT_EQ(h.openVote(FEEDREG_VAR_DEPOSIT, 5000, SH1, FEEDREG_DEFAULT_PROPOSAL_FEE - 1), INVALID_PROPOSAL_INDEX);  // fee short
    EXPECT_EQ(bal(SH1), before);
    sint64 c = bal(CAROL);
    EXPECT_EQ(h.openVote(FEEDREG_VAR_DEPOSIT, 5000, CAROL), INVALID_PROPOSAL_INDEX);             // not a shareholder
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
    EXPECT_NE(h.openVote(FEEDREG_VAR_DEPOSIT, 5000, SH1, FEEDREG_DEFAULT_PROPOSAL_FEE + 77), INVALID_PROPOSAL_INDEX);
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
    EXPECT_FALSE(h.access(id1, CONSUMER).active);
    EXPECT_EQ(h.buy(id1, 1).returnCode, FEEDREG_ERR_WRONG_STATUS);
    EXPECT_EQ(h.openVote(FEEDREG_VAR_RETIRE_FEED, (sint64)id1), INVALID_PROPOSAL_INDEX);
    EXPECT_EQ(h.openVote(FEEDREG_VAR_APPROVE_FEED, (sint64)id1), INVALID_PROPOSAL_INDEX);       // no way back
    EXPECT_EQ(h.propose(frankfurter()).feedId, 2u);              // can be proposed afresh
    h.invariant();
}

TEST(Governance, ParametersChangeByVote)
{
    H h;
    h.pass(FEEDREG_VAR_DEPOSIT, 5000);
    h.endEpoch();
    h.pass(FEEDREG_VAR_PROPOSER_SHARE_BPS, 0);
    h.endEpoch();
    h.pass(FEEDREG_VAR_PROPOSAL_FEE, 0);
    h.endEpoch();
    R::GetInfo_input gi{};
    auto info = h.GetInfo(gi);
    EXPECT_EQ(info.proposeDeposit, 5000);
    EXPECT_EQ(info.proposerShareBps, 0);
    EXPECT_EQ(info.proposalFee, 0);
    auto o = h.propose(frankfurter(), ALICE, 0, NULL_ID, 5000);
    EXPECT_EQ(o.returnCode, FEEDREG_OK);
    EXPECT_EQ(h.feed(o.feedId).deposit, 5000);
    EXPECT_NE(h.openVote(FEEDREG_VAR_APPROVE_FEED, (sint64)o.feedId, SH1, 0), INVALID_PROPOSAL_INDEX);
    h.invariant();
}

TEST(Governance, DepositChangeDoesNotTouchHeldDeposits)
{
    H h;
    auto o = h.propose(frankfurter());
    h.pass(FEEDREG_VAR_DEPOSIT, 5000);
    h.endEpoch();
    h.pass(FEEDREG_VAR_APPROVE_FEED, (sint64)o.feedId);
    sint64 before = bal(ALICE);
    h.endEpoch();
    EXPECT_EQ(bal(ALICE), before + FEEDREG_DEFAULT_DEPOSIT);     // the deposit actually paid
    h.invariant();
}

// ============================================================ access

TEST(Access, PendingFeedHasNoAccess)
{
    H h;
    auto o = h.propose(frankfurter(), ALICE, 0);
    EXPECT_FALSE(h.access(o.feedId, CONSUMER).active);
    sint64 before = bal(CONSUMER);
    EXPECT_EQ(h.buy(o.feedId, 1, CONSUMER, 10).returnCode, FEEDREG_ERR_WRONG_STATUS);
    EXPECT_EQ(bal(CONSUMER), before);
    EXPECT_EQ(h.buy(99, 1, CONSUMER, 0).returnCode, FEEDREG_ERR_NOT_FOUND);
    EXPECT_FALSE(h.access(99, CONSUMER).active);
    EXPECT_FALSE(h.access(0, CONSUMER).active);
}

TEST(Access, FreeOpenFeedNeedsNoPurchase)
{
    H h;
    uint64 f = h.approved(frankfurter());
    auto a = h.access(f, CONSUMER);
    EXPECT_TRUE(a.active);
    EXPECT_EQ(a.untilEpoch, 65535);
}

TEST(Access, PaidFeedSplitsFeeAndExpires)
{
    H h;
    uint64 f = h.approved(frankfurter(), 1000);
    EXPECT_FALSE(h.access(f, CONSUMER).active);
    sint64 alice = bal(ALICE), consumer = bal(CONSUMER);
    uint16 e = chain().epoch;
    auto o = h.buy(f, 3, CONSUMER, 3000 + 50);                     // 50 too much
    ASSERT_EQ(o.returnCode, FEEDREG_OK);
    EXPECT_EQ(o.paid, 3000);
    EXPECT_EQ(o.untilEpoch, e + 2);                                // this epoch and the next two
    EXPECT_EQ(bal(CONSUMER), consumer - 3000);
    EXPECT_EQ(bal(ALICE), alice + 600);                            // 20% to the proposer
    EXPECT_EQ(h.s->dividendPool, 2400);
    h.invariant();
    EXPECT_TRUE(h.access(f, CONSUMER).active);
    EXPECT_FALSE(h.access(f, CONSUMER2).active);
    h.endEpoch();
    EXPECT_EQ(h.s->dividendPool, 2400 - 676 * 3);                  // 3 QU per share paid out
    EXPECT_EQ(chain().dividends, 676 * 3);
    EXPECT_TRUE(h.access(f, CONSUMER).active);
    h.endEpoch();
    EXPECT_TRUE(h.access(f, CONSUMER).active);
    h.endEpoch();
    EXPECT_FALSE(h.access(f, CONSUMER).active);
    EXPECT_EQ(h.s->access.population(), 0u);                       // swept
    h.invariant();
}

TEST(Access, ExtendingAddsToTheEnd)
{
    H h;
    uint64 f = h.approved(frankfurter(), 10);
    uint16 e = chain().epoch;
    EXPECT_EQ(h.buy(f, 2).untilEpoch, e + 1);
    EXPECT_EQ(h.buy(f, 3).untilEpoch, e + 4);
    h.endEpoch();
    EXPECT_EQ(h.buy(f, 1).untilEpoch, e + 5);
    EXPECT_EQ(h.buy(f, FEEDREG_MAX_ACCESS_EPOCHS).returnCode, FEEDREG_ERR_EPOCHS);   // more than 52 ahead
    EXPECT_EQ(h.access(f, CONSUMER).untilEpoch, e + 5);
    h.invariant();
}

TEST(Access, BadPurchasesAreRefunded)
{
    H h;
    uint64 f = h.approved(frankfurter(), 1000);
    sint64 before = bal(CONSUMER);
    EXPECT_EQ(h.buy(f, 2, CONSUMER, 1999).returnCode, FEEDREG_ERR_PAYMENT);
    EXPECT_EQ(h.buy(f, 0, CONSUMER, 5).returnCode, FEEDREG_ERR_EPOCHS);
    EXPECT_EQ(h.buy(f, FEEDREG_MAX_ACCESS_EPOCHS + 1).returnCode, FEEDREG_ERR_EPOCHS);
    EXPECT_EQ(h.buy(f, 65535, CONSUMER, 0).returnCode, FEEDREG_ERR_EPOCHS);
    EXPECT_EQ(bal(CONSUMER), before);
    EXPECT_EQ(h.buy(f, FEEDREG_MAX_ACCESS_EPOCHS).returnCode, FEEDREG_OK);
    h.invariant();
}

TEST(Access, RestrictedFeedServesOnlyItsUser)
{
    H h;
    uint64 f = h.approved(frankfurter(), 0, CONSUMER);
    EXPECT_FALSE(h.access(f, CONSUMER).active);                    // even the named user registers first
    sint64 before = bal(CONSUMER2);
    EXPECT_EQ(h.buy(f, 1, CONSUMER2, 100).returnCode, FEEDREG_ERR_RESTRICTED);
    EXPECT_EQ(bal(CONSUMER2), before);
    EXPECT_EQ(h.buy(f, 1, CONSUMER).returnCode, FEEDREG_OK);
    EXPECT_TRUE(h.access(f, CONSUMER).active);
    EXPECT_FALSE(h.access(f, CONSUMER2).active);
    h.invariant();
}

TEST(Access, ProposerShareRoundsDown)
{
    H h;
    uint64 f = h.approved(frankfurter(), 7);
    sint64 alice = bal(ALICE);
    h.buy(f, 1);
    EXPECT_EQ(bal(ALICE), alice + 1);                              // 7 * 2000 / 10000 = 1.4
    EXPECT_EQ(h.s->dividendPool, 6);
    h.invariant();
}

TEST(Access, TableFull)
{
    H h;
    uint64 f = h.approved(frankfurter(), 1);
    for (uint64 i = 0; i < FEEDREG_MAX_ACCESS; i++)
    {
        id u(1000 + i, 9, 9, 9); chain().balances[u] = 10;
        ASSERT_EQ(h.buy(f, 1, u).returnCode, FEEDREG_OK) << i;
    }
    sint64 before = bal(CONSUMER);
    EXPECT_EQ(h.buy(f, 1, CONSUMER).returnCode, FEEDREG_ERR_ACCESS_FULL);
    EXPECT_EQ(bal(CONSUMER), before);
    id first(1000, 9, 9, 9);
    EXPECT_EQ(h.buy(f, 1, first).returnCode, FEEDREG_OK);         // extending an existing entry still works
    h.invariant();
    h.endEpoch();
    EXPECT_EQ(h.buy(f, 1, CONSUMER).returnCode, FEEDREG_OK);       // room again after the sweep
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
    EXPECT_EQ(p1.feeds.get(3).feePerEpoch, 3);
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
    EXPECT_EQ(s.valueType, FEEDREG_VALUE_DECIMAL);
    EXPECT_EQ(s.decimals, 6);
}

// ============================================================ a longer run

TEST(Scenario, MoneyIsAlwaysAccountedFor)
{
    H h;
    uint64 seed = 12345;
    auto rnd = [&](uint64 n) { seed = seed * 6364136223846793005ULL + 1442695040888963407ULL; return (seed >> 33) % n; };
    for (int step = 0; step < 3000; step++)
    {
        switch (rnd(6))
        {
        case 0: h.propose(withUrl("https://example.com/s" + std::to_string(rnd(40))), rnd(2) ? ALICE : BOB, (sint64)rnd(3) * 100, NULL_ID, FEEDREG_DEFAULT_DEPOSIT + (sint64)rnd(3)); break;
        case 1: { R::WithdrawFeed_input in{rnd(h.s->feedCount + 2)}; h.WithdrawFeed(in, rnd(2) ? ALICE : BOB); break; }
        case 2: if (h.s->feedCount) { uint64 f = 1 + rnd(h.s->feedCount); uint16 i = h.openVote(rnd(2) ? FEEDREG_VAR_APPROVE_FEED : FEEDREG_VAR_RETIRE_FEED, (sint64)f); if (i != INVALID_PROPOSAL_INDEX) { h.vote(SH1, i, 1); h.vote(SH2, i, 1); } } break;
        case 3: if (h.s->feedCount) h.buy(1 + rnd(h.s->feedCount), (uint16)rnd(5), rnd(2) ? CONSUMER : CONSUMER2, (sint64)rnd(1000)); break;
        case 4: if (rnd(10) == 0) h.endEpoch(); break;
        default: break;
        }
        h.invariant();
        if (::testing::Test::HasFailure()) { ADD_FAILURE() << "step " << step; return; }
    }
    EXPECT_GT(h.s->feedCount, 10u);
}
