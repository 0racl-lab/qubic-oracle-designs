# HttpRead: a plan for reading web APIs from Qubic contracts

> **Status:** plan and draft interface for discussion. Not implemented, not reviewed by the core or oracle team.
> Draft header: [`oracle_interfaces/HttpRead.h`](../oracle_interfaces/HttpRead.h). It compiles against qubic/core and passes contract-verify.

**In one line:** a contract asks for one value from an approved public web API (an exchange rate on a date, a day's rainfall, a final match score), and the computors only accept it if their oracle machines got the same answer.

This is the "curl oracle" people ask for, made safe enough for computors to run. The rest of this note explains why a literal curl can't work, and what can.

## What it would unlock

- **Fiat-priced contracts:** offers priced in GBP or JPY using a published reference rate for a given day.
- **Weather and parametric cover:** pay out if rainfall at a location on a date was above a threshold.
- **Event outcomes:** settle on a final, published result, such as a match score or an election tally from an official source.
- **Official lists:** check an id against a list a regulator or issuer publishes.

Some of these, prediction markets and betting in particular, fall under gambling rules in the UK and many other places. They belong in their own contracts with their own legal review, not inside unrelated projects.

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
| **Trust** | The oracle can only prove "the site said X", not that X is true |

So an "any URL" oracle would rarely reach agreement, and would be dangerous when it did.

## Design principles

1. **No raw URLs. Sources are code.** Each source is an **adapter**: a small, reviewed module in the oracle machine that knows exactly one API. It knows the base URL, the endpoints, how to validate each parameter, how to tell "no data" from "error", and how to normalise the response. A query names a source and an endpoint, never a URL. Adding a source is a pull request, the same as adding an interface. This mirrors how Price already works, with a source id such as `binance` or `mexc` chosen in the query.
2. **Every answer is pinned.** Phase 1 allows only two kinds of endpoint:
   - **Historical values** for a past timestamp, which the source no longer changes: a reference rate for a closed day, an archived weather value.
   - **Final outcomes**, where the adapter checks the source's own "final" or "finished" flag and answers `NOT_FINAL` until then.

   Live values ("the price now") are left for phase 3.
3. **One field out, exactly.** The adapter normalises the response. The query picks one field and a type: a decimal (scaled by 10^decimals and rounded half-to-even from the **response text**, never via floating point), text (up to 128 bytes) or a boolean.
4. **Fail closed.** A timeout, an error status or a response that doesn't match the adapter's expected shape makes the machine **abstain**, so nothing is committed. Only a definitive "this doesn't exist" from the source becomes `NO_DATA`. Each machine fetches twice from independent hosts or mirrors where the source offers them, and abstains if the two differ.
5. **Public data only, no keys in queries.** If a source needs a key, every operator holds their own key on their own machine, and the adapter must produce identical output whoever's key is used. Sources that need keys are discouraged in phase 1.

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
| A sports results source | Final scores by event id | Most asked-for use | Most need paid keys and forbid redistribution; may be better as signed data from a provider (see below) |

## Who decides which sources exist

- **Phase 1:** the source list is code. A new adapter is a pull request to oracle-machine, plus its id in the core header. Core developers review it, and it goes live when computors run the release. That is slow, but every machine runs the same adapters, which is what agreement needs.
- **Later:** a governed registry. That could be a contract whose shareholders vote sources in and out, which oracle machines read before answering. It needs a way for machines to read contract state and a migration path, so it should come after phase 1 has proved itself.

## Phases

| Phase | Delivers | Needs |
|---|---|---|
| **0. Agree** | Confirm with the core and oracle team that a narrow HttpRead is wanted, and agree the interface shape and fee | This note; a dev-channel thread |
| **1. Historical values** | The `HttpRead` interface; a shared `read_http` service with the double fetch and agreement checks; Frankfurter and Open-Meteo archive adapters; recorded-response selftests; core tests in the style of this repo's other interfaces | No core engine change |
| **2. Final outcomes** | `NOT_FINAL` handling; first event-result adapter; guidance for contracts on using two independent sources and requiring them to agree | A source with suitable terms |
| **3. Live values and governance** | Median agreement for numbers that move. Machines commit values and the engine takes the median of a quorum, rather than requiring identical bytes. Plus the governed source registry | **A core oracle-engine change**: the biggest step, and the core team's call |

## How it fits with the other oracles

- **ContentRead:** if the data can be published as a file (a results list, a dataset), publishing it and reading it by hash is simpler and stronger than any web read.
- **ProofVerify (zkTLS):** for data behind a login, like a bank balance or a private account, a zkTLS proof lets the user prove what a website showed them without sharing credentials. HttpRead can only ever read public data.
- **Signed data:** a provider that signs its results with a Qubic key needs no oracle at all, because contracts can check signatures with `qpi.signatureValidity`. That suits paid data such as sports feeds. The trade-off is trusting one signer instead of the computor quorum.

## Open questions for the core and oracle team

- [ ] Is a narrow, adapter-based HttpRead wanted, or would you rather have a separate small interface per domain (FX, weather, results)?
- [ ] Should adapters live in one `read_http` service or one service each?
- [ ] Is the double fetch per machine worth its cost, or is cross-machine agreement enough?
- [ ] Is median agreement for live numbers (phase 3) on the engine roadmap at all?
- [ ] What fee range would computors accept for web reads?
