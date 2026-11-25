# HttpRead: a plan for reading web APIs from Qubic contracts

> **Status:** plan and draft interface for discussion. Not implemented, not reviewed by the core or oracle team.
> Draft header: [`oracle_interfaces/HttpRead.h`](../oracle_interfaces/HttpRead.h). It compiles against qubic/core and passes contract-verify.

**In one line:** a contract asks for one value from an approved public web API (an exchange rate on a date, a day's rainfall, an official published figure), and computors accept the reply only if a quorum of their oracle machines read the same thing.

This is the "curl oracle" people ask for, made safe enough for computors to run. The rest of this note explains why a literal curl can't work, and what can.

## What it would unlock

- **Fiat-priced contracts:** offers priced in GBP or JPY using a published reference rate for a given day.
- **Weather-linked contracts:** pay out if recorded rainfall at a location on a date was above a threshold.
- **Official figures:** read a statistic, index or list that a central bank, agency or issuer has already published.

**What it is not.** Like every Qubic oracle, HttpRead has no concept of truth. A reply means one thing only: a quorum of computors' oracle machines agreed on what the source said. It reads data that already exists. It doesn't judge it, and it doesn't predict anything. Where one provider isn't reliable enough, the answer is to combine several providers inside the oracle machine (see principle 6), the way Price's `gate_mexc` source averages gate.io and MEXC.

**Who it's for.** As with the other interfaces, the main users are **contracts** querying data when their logic needs it. User-sent queries mainly serve developers testing their integrations.

## Why a raw curl oracle fails

Every oracle machine answers independently, and computors accept a reply only when a quorum agree on it **byte for byte**. The open web breaks that in several ways:

| Problem | Example |
|---|---|
| **Timing** | A price fetched one second apart is different |
| **Location** | CDNs serve different versions to different regions |
| **Noise** | Timestamps, request ids and ads change on every load |
| **Blocking** | Rate limits and bot protection let some machines through and not others |
| **Secrets** | Queries are public on-chain, so an API key in a query is leaked to everyone |
| **Abuse** | A contract could aim hundreds of machines at a site (a denial-of-service), or at internal addresses |
| **Trust** | The oracle can only report "the site said X"; whether X is right is up to the source |

So an "any URL" oracle would rarely reach agreement, and would be dangerous when it did.

## Design principles

1. **No raw URLs. Sources are code.** Each source is an **adapter**: a small, reviewed module in the oracle machine that knows exactly one API. It knows the base URL, the endpoints, how to validate each parameter, how to tell "no data" from "error", and how to normalise the response. A query names a source and an endpoint, never a URL. Adding a source is a pull request, the same as adding an interface. This mirrors how Price already works, with a source id such as `binance` or `mexc` chosen in the query.
2. **Every answer is pinned.** Phase 1 allows only two kinds of endpoint:
   - **Historical values** for a past timestamp, which the source no longer changes: a reference rate for a closed day, an archived weather value.
   - **Final figures**, where the adapter checks the source's own "final" flag (a published statistic that is no longer provisional) and answers `NOT_FINAL` until then.

   Live values ("the price now") are left for phase 3.
3. **One field out, exactly.** The adapter normalises the response. The query picks one field and a type: a decimal (scaled by 10^decimals and rounded half-to-even from the **response text**, never via floating point), text (up to 128 bytes) or a boolean.
4. **Fail closed.** A timeout, an error status or a response that doesn't match the adapter's expected shape makes the machine **abstain**, so nothing is committed. Only a definitive "this doesn't exist" from the source becomes `NO_DATA`. Each machine fetches twice from independent hosts or mirrors where the source offers them, and abstains if the two differ.
5. **Public data only, no keys in queries.** If a source needs a key, every operator holds their own key on their own machine, and the adapter must produce identical output whoever's key is used. Sources that need keys are discouraged in phase 1.
6. **Combine sources inside the oracle machine, not in contracts.** An adapter can be a *combined source* that reads several providers for the same pinned moment and averages them, with deterministic rounding. This follows the existing Price oracle: its `gate_mexc` source averages gate.io and MEXC for QUBIC/USDT, and its `binance_mexc` and `binance_gate` sources do the same. The contract still sends one query and gets one value.

## Draft interface

| Query field | Size | Meaning |
|---|---|---|
| `source` | 32 | Source id, e.g. `frankfurter`, `openmeteo_ar` (as Price uses `binance`) |
| `endpoint` | 8 | Endpoint number within the source |
| `timestamp` | 8 | `DateAndTime` the value is for, or the event's scheduled time |
| `params` | 128 | Up to 4 × 32-byte ASCII parameters, validated by the endpoint's rule |
| `field` | 64 | Dot path in the adapter's normalised response |
| `valueType` | 8 | 1 decimal, 2 text, 3 boolean |
| `decimals` | 8 | Scaling for decimals (0–18) |

Query: 256 bytes. Reply: 160 bytes, made up of `code`, `valueType`, `intValue` (sint64), `textLen` and 128 bytes of `text`.

**Result codes:** `SUCCESS`, `BAD_QUERY`, `SOURCE_UNSUPPORTED`, `NO_DATA`, `NOT_FINAL` (retriable), `NO_FIELD`, `TYPE_MISMATCH`, `VALUE_TOO_LARGE`.

**Fee:** 200 QU as a starting point, to be set with the core team. Web reads cost oracle machines bandwidth and count against each source's rate limits.

## What an adapter looks like (oracle-machine side)

A sketch of the Python an adapter would provide to a shared `read_http` service. The service owns everything else: the framing, the double fetch, the agreement checks and the reply encoding.

```python
class Frankfurter(Adapter):
    source_id = "frankfurter"
    hosts = ["https://api.frankfurter.dev"]          # operators may add a self-hosted mirror

    # endpoint 1: ECB reference rate for a closed day
    # params: [base currency, quote currency], each ^[A-Z]{3}$; timestamp: the date (UTC)
    def endpoint_1(self, ts, base, quote):
        day = ts.date()
        if day >= utc_today():                       # not final until the day is over
            return NotFinal()
        doc = self.get_json(f"/v2/rates?providers=ECB&date={day}&base={base}&quotes={quote}")
        row = self.only_row(doc, date=day, base=base, quote=quote)   # holiday: no row for that date
        if row is None:
            return NoData()
        return {"rate": row.decimal_text("rate")}    # kept as text: scaled exactly later
```

Each adapter ships with recorded responses, both real and edge cases, and a selftest, the same way the read_evm_log, read_utxo_tx and read_content services do.

## Launch candidates

All of these need their terms and their exact behaviour confirmed before an adapter is written.

| Source | What | Why it fits | To check |
|---|---|---|---|
| **Frankfurter** (ECB reference rates) | Daily FX reference rates, history back decades | Free, no key, open source, self-hostable, commercial use allowed | Pin to the ECB provider only; weekends and holidays have no new rate; only answer closed days |
| **Open-Meteo archive** | Daily weather by coordinate and date (ERA5 reanalysis) | No key, history back to 1940, CC BY 4.0 data | Free tier is **non-commercial**, so commercial contracts need a subscription; recent days may be revised, so only answer dates older than a set lag |
| A combined FX source | The same rate averaged across two central-bank providers available through Frankfurter | Follows Price's `gate_mexc` pattern; no single provider decides | Rates must be for the same closed day; agree rounding |

## Who decides which sources exist

- **Phase 1:** the source list is code. A new adapter is a pull request to oracle-machine, plus its id in the core header. Core developers review it, and it goes live when computors run the release. That is slow, but every machine runs the same adapters, which is what agreement needs.
- **Later:** feeds governed by a registry contract, so adding a feed is a shareholder vote rather than a core release. See [Feeds governed by a registry contract](#feeds-governed-by-a-registry-contract) below.

## Feeds governed by a registry contract

Phase 1 puts every source in reviewed code, so each new feed waits for a release. The step after that is to let feeds be **voted in**, while keeping the fetching itself in reviewed code. With HttpRead live, this needs one contract and one more HttpRead source, not a new oracle interface.

A sketch of the contract is in [`contracts/FeedRegistry.h`](../contracts/FeedRegistry.h), written as if HttpRead were live. It has 53 native tests and a mutation check (see [Checks](../README.md#checks)), passes contract-verify and type-checks against core's QPI headers together with [`HttpRead.h`](../oracle_interfaces/HttpRead.h). It has not been proposed to core.

### Part 1: the FeedRegistry contract

A normal Qubic contract, proposed, voted and IPO'd like any other. It keeps the approved feeds, and it is **the only way to read them**: contracts ask the registry, and the registry sends the HttpRead query.

**What a feed definition holds.** Fixed size (396 bytes), checked when the feed is proposed, never changed afterwards:

| Field | Meaning |
|---|---|
| `urlTemplate` (256 B) | `https://` URL with placeholders `{0}`..`{3}` in the path or query, e.g. `https://api.frankfurter.app/{0}?from={1}&to={2}` |
| `paramKinds` (4 × 1 B) | What each placeholder accepts, from a fixed list: ISO date, currency code, integer, decimal, slug (`[a-z0-9-]{1,32}`), coordinate. **No free text and no regular expressions** |
| `timePolicy`, `minAgeSeconds` | `historical`: only for moments at least `minAgeSeconds` old (1 minute to 31 days). `final`: only when the boolean at `finalPath` is true; the minimum age may be 0 |
| `valuePath` (64 B) | JSON pointer (RFC 6901) to the value, may use the placeholders, e.g. `/rates/{2}` |
| `finalPath` (64 B) | `final` feeds only: JSON pointer to the source's "final" flag |
| `valueType`, `decimals` | HttpRead's value types: decimal (0 to 18 decimals, so whole numbers too), text or boolean |

**What the contract checks** before it accepts a definition:

- the URL starts with `https://`;
- the host is a lower-case DNS name with at least one dot, and its last label is letters only. That rules out IP addresses, `localhost`, ports and `user@host`;
- the path and query use only URL-safe characters, valid `%XX` escapes and declared placeholders, and every declared parameter is used. Placeholders can't appear in the host;
- the parameter kinds are known and have no gaps;
- every unused byte is zero, so a definition has exactly one encoding and one hash.

Around the definition, the entry stores `definitionHash` (K12 of the 396 bytes, the hash QPI offers), the proposer, an optional `restrictedTo` contract, an optional `feePerUse`, the deposit while pending, the status and a count of answered requests.

**Lifecycle.**

| From | Event | To |
|---|---|---|
| | `ProposeFeed`, with the deposit | pending |
| pending | shareholder vote "approve feed N" accepted at the end of an epoch | approved, deposit refunded |
| pending | no accepted vote within 4 epochs | expired, deposit burned |
| pending | `WithdrawFeed` by the proposer | withdrawn, half the deposit refunded, half burned |
| approved | shareholder vote "retire feed N" accepted | retired, for good |

**The deposit starts at 1 billion QU** (a voted parameter, 10 million to 10 trillion). A feed is a standing claim on every oracle machine's bandwidth and on someone else's API, so proposing one should cost real money. An honest proposer gets it all back on approval; a proposal nobody votes for loses it.

Feed ids are never reused. The same definition can't be live twice; once retired, expired or withdrawn it can be proposed again under a new id.

**Votes** use QPI's standard shareholder yes/no proposals, so wallets and explorers that already show them work unchanged. Variables: approve feed, retire feed, deposit, proposer's share, proposal fee.

**Reading a value.** A consumer contract calls `RequestValue(feedId, timestamp, params)` with the HttpRead query fee plus the feed's `feePerUse` (`GetFeed` returns the total). The registry:

1. checks the feed is approved, the caller is allowed (`restrictedTo`), the timestamp is a valid date at least `minAgeSeconds` old, and each parameter matches its kind (a real calendar date, three capital letters, a number in range, and so on). Anything wrong is refunded in full before any oracle fee is spent;
2. sends an HttpRead query for the **`feedreg` source**: `endpoint` is the request id, `timestamp` and `params` are copied, `valueType` and `decimals` are the feed's;
3. records the request, including the oracle query id, and holds the feed fee.

When HttpRead answers, the reply is stored with the request and the consumer reads it with `GetRequest(requestId)`. Contracts can only call contracts with a lower index, so consumers are deployed after the registry, and they read the answer rather than being called back. The last 1,024 requests are kept.

**Who earns.** The oracle fee goes to the oracle as usual. The feed fee is held until the oracle answers. On an answer (including the source saying "no data") the proposer gets 20% (a voted parameter, at most 50%) and shareholders get the rest as dividends. If the oracle times out or can't agree, the feed fee is refunded.

### Part 2: the `feedreg` source in HttpRead

One more source in the oracle machine's `read_http` service, documented in [`HttpRead.h`](../oracle_interfaces/HttpRead.h). For this source each oracle machine:

1. Reads the request with the registry's `GetRequest(endpoint)` function through the operator's **own bob node**, using bob's `qubic_querySmartContract` call. This is how oracle machines already read Qubic data: the existing `read_qubic_log` service reads receipts from the operator's own bob (`BOB_URLS`), and the core-to-oracle-machine connection itself only carries queries and replies ([`core_om_network_messages.h`](https://github.com/qubic/core/blob/main/src/oracle_core/core_om_network_messages.h)). The registry writes the request in the same tick it sends the query, and bob follows the chain a little behind, so if the record isn't there yet the machine **waits a second and reads again, for up to 10 seconds**, then abstains. Each retry uses a new bob nonce, because bob caches a result per nonce. All of this fits well inside the 120-second query timeout.
2. Serves the query only if the request is pending, **its oracle query id is this query's id**, and its timestamp and parameters equal the query's. A record that exists but doesn't match is answered `SOURCE_UNSUPPORTED`; every machine reads the same record, so they agree. A query sent straight to HttpRead, bypassing the registry and its fee, has no record at all: machines abstain and it times out, with nothing fetched. This is what makes fees and restrictions hold: the registry knows who called it, while an oracle machine is never told who sent a query.
3. Reads the feed with `GetFeed` (also through bob) and checks that its stored definition hashes to the `definitionHash` recorded with the request. A stale or tampered read can only cause a refusal, never a fetch with a different definition.
4. Checks the parameters against their kinds again, fills in the template and fetches under the service's **hard-coded safety policy**:
   - GET only;
   - the host must resolve to a public address, so no private, loopback or link-local addresses;
   - no redirects to another host;
   - size and time caps, and a rate limit per domain.

   A vote can't loosen any of these.
5. Extracts the value exactly as the definition says (text-based decimals, no floating point), or abstains on any error or disagreement.

### Why this split

- **New feeds don't need a core release.** A shareholder vote approves them.
- **The dangerous parts stay in reviewed code:** network access, timeouts, extraction and agreement.
- **No new oracle interface.** The registry reuses HttpRead's query and reply as they are.
- **The definition hash keeps machines in agreement** even while the registry changes. Every machine either uses exactly the definition the request was paid for, or refuses.
- **It still only suits feeds that can answer "value at time T", or that publish final figures.** A vote can't make a constantly changing page agree across hundreds of machines. Shareholders should check this before approving a feed.

### What has to be agreed first

- **Oracle machines reading contract state.** Services already read Qubic data from the operator's own bob (`read_qubic_log`), and bob already offers `qubic_querySmartContract`, so this needs no core or bob change. The new part is only that a service decides what to fetch from contract state rather than from a log. Because of the query id and hash checks, and the retry, a stale read can only delay or refuse an answer, never cause a wrong fetch. It's still the oracle team's call.
- **Order of work:** HttpRead and its `read_http` service first (phases 1 to 3). Then the `feedreg` source and the FeedRegistry contract proposal together, since neither is useful alone.
- **Responsibility:** a voted-in feed is only as good as its source. Shareholders take on the job of not approving feeds that misbehave, and of retiring ones that do.

## Phases

| Phase | Delivers | Needs |
|---|---|---|
| **0. Agree** | Confirm with the core and oracle team that a narrow HttpRead is wanted, and agree the interface shape and fee | This note; a dev-channel thread |
| **1. Historical values** | The `HttpRead` interface; a shared `read_http` service with the double fetch and agreement checks; Frankfurter and Open-Meteo archive adapters; recorded-response selftests; core tests in the style of this repo's other interfaces | No core engine change |
| **2. Combined sources and finality** | Combined-source adapters (several providers averaged inside the machine, like `gate_mexc`); `NOT_FINAL` handling for figures that are published once final | Sources with suitable terms |
| **3. Moving values** | Values that change all day, handled the way Price handles prices: pinned to an exact timestamp, from providers that can answer "the value at time T", combined across providers | No engine change while answers stay byte-identical |
| **4. Voted-in feeds** | The `feedreg` source in `read_http` and the FeedRegistry contract (see above) | Core team agreement that oracle machines may read contract state; a contract proposal and vote |

## How it fits with the other oracles

- **ContentRead:** if the data can be published as a file (a results list, a dataset), publishing it and reading it by hash is simpler and stronger than any web read.
- **Zero-knowledge proofs (zkTLS):** for data behind a login, like a bank balance or a private account, a zkTLS proof lets the user prove what a website showed them without sharing credentials. HttpRead can only ever read public data.
- **Signed data:** a provider that signs its results with a Qubic key needs no oracle at all, because contracts can check signatures with `qpi.signatureValidity`. That suits paid or licensed data. The trade-off is trusting one signer instead of the computor quorum.

## Open questions for the core and oracle team

- [ ] Is a narrow, adapter-based HttpRead wanted, or would you rather have a separate small interface per domain (FX, weather, results)?
- [ ] Should adapters live in one `read_http` service or one service each?
- [ ] Is the double fetch per machine worth its cost, or is cross-machine agreement enough?
- [ ] Should combined sources (as with Price's `gate_mexc`) be the default for anything numeric?
- [ ] Would an oracle service be allowed to depend on a registry contract's (public) state to decide what to fetch, given the definition is pinned by hash in each query?
- [ ] What fee range would computors accept for web reads?
