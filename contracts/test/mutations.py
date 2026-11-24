#!/usr/bin/env python3
"""Mutation check: each mutation below breaks one rule of FeedRegistry.h, and the
test suite must fail for every one of them.
Usage: mutations.py "<build command>"   The command builds the tests against the header
in $FEEDREG_DIR into the binary $OUT. A mutation that does not compile is reported as
an error, not as caught."""
import os, shutil, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "FeedRegistry.h")

# Left out on purpose, because no test can tell them apart from the original:
# - ValidateUrl "{n}" bound (>= vs >): a placeholder one past the last parameter also fails
#   the "every parameter used exactly as declared" mask check, with the same error code.
#   The bound stays because it keeps the shift in range.
# - GetAccess "untilEpoch >= epoch": END_EPOCH removes entries as they run out, so a stored
#   entry is always current. The comparison stays as a guard.
M = [
    # definition validation
    ("input.url.get(4) != FEEDREG_CH_S", "false"),
    ("input.url.get(7) != FEEDREG_CH_SLASH)", "false)"),
    ("if (locals.i == 8 || locals.prev == FEEDREG_CH_DOT || locals.prev == FEEDREG_CH_DASH)\n                {\n                    return;", "if (false)\n                {\n                    return;"),
    ("locals.prev == FEEDREG_CH_DOT || locals.prev == FEEDREG_CH_DASH)\n                {", "locals.prev == FEEDREG_CH_DOT)\n                {"),
    ("if (locals.i == 8 || locals.prev == FEEDREG_CH_DOT)\n                {", "if (locals.prev == FEEDREG_CH_DOT)\n                {"),
    ("locals.dots == 0 ||", ""),
    ("locals.hostEnd - locals.lastDot < 3", "locals.hostEnd - locals.lastDot < 2"),
    ("if (locals.c < FEEDREG_CH_LOWER_A || locals.c > FEEDREG_CH_LOWER_Z)\n            {\n                return;", "if (false)\n            {\n                return;"),
    ("(locals.c >= FEEDREG_CH_LOWER_A && locals.c <= FEEDREG_CH_LOWER_Z) || (locals.c >= FEEDREG_CH_0", "(locals.c >= FEEDREG_CH_UPPER_A && locals.c <= FEEDREG_CH_LOWER_Z) || (locals.c >= FEEDREG_CH_0"),
    ("input.url.get(locals.hostEnd) != FEEDREG_CH_SLASH ||", ""),
    ("|| input.url.get(255) != 0)", ")"),
    ("            if (locals.ended)\n            {\n                if (locals.c != 0)\n                {\n                    return;\n                }\n                continue;\n            }\n            if (locals.c == 0)\n            {\n                locals.ended = true;\n                continue;\n            }\n            locals.pc.c = locals.c;\n            CALL(IsPathChar, locals.pc, locals.pcOut);\n            if (locals.pcOut.ok || locals.c == FEEDREG_CH_SLASH || locals.c == FEEDREG_CH_QUESTION",
     "            if (locals.c == 0)\n            {\n                locals.ended = true;\n                continue;\n            }\n            locals.pc.c = locals.c;\n            CALL(IsPathChar, locals.pc, locals.pcOut);\n            if (locals.pcOut.ok || locals.c == FEEDREG_CH_SLASH || locals.c == FEEDREG_CH_QUESTION"),
    ("|| locals.c == FEEDREG_CH_PLUS || locals.c == FEEDREG_CH_TILDE)", "|| locals.c == FEEDREG_CH_PLUS || locals.c == FEEDREG_CH_TILDE || locals.c == 64)"),
    ("|| locals.c == FEEDREG_CH_PLUS || locals.c == FEEDREG_CH_TILDE)", "|| locals.c == FEEDREG_CH_PLUS || locals.c == FEEDREG_CH_TILDE || locals.c == 35)"),
    ("|| input.c == FEEDREG_CH_DASH || input.c == FEEDREG_CH_DOT || input.c == FEEDREG_CH_UNDERSCORE;", "|| input.c == FEEDREG_CH_DASH || input.c == FEEDREG_CH_DOT || input.c == FEEDREG_CH_UNDERSCORE || input.c == 32;"),
    ("|| (input.c >= FEEDREG_CH_LOWER_A && input.c <= FEEDREG_CH_LOWER_F);", "|| (input.c >= FEEDREG_CH_LOWER_A && input.c <= FEEDREG_CH_LOWER_Z);"),
    ("locals.used |= (uint8)(1 << (locals.d - FEEDREG_CH_0));", "locals.used |= 1;"),
    ("if (locals.used != (uint8)((1 << input.paramCount) - 1))", "if (false)"),
    ("if (locals.i + 2 > 254 || input.url.get(locals.i + 2) != FEEDREG_CH_RBRACE)", "if (locals.i + 2 > 254)"),
    ("if (locals.ended || locals.k >= FEEDREG_PARAM_KIND_COUNT)", "if (locals.k >= FEEDREG_PARAM_KIND_COUNT)"),
    ("if (locals.ended || locals.k >= FEEDREG_PARAM_KIND_COUNT)", "if (locals.ended || locals.k > FEEDREG_PARAM_KIND_COUNT)"),
    ("if (!locals.pathOut.ok || locals.pathOut.empty)", "if (!locals.pathOut.ok)"),
    ("if (input.path.get(0) != FEEDREG_CH_SLASH || input.path.get(63) != 0)", "if (input.path.get(63) != 0)"),
    ("if (locals.d != FEEDREG_CH_0 && locals.d != FEEDREG_CH_1)", "if (false)"),
    ("if (locals.i + 2 > 62 || input.path.get(locals.i + 2) != FEEDREG_CH_RBRACE)\n                {\n                    return;\n                }\n                locals.d = input.path.get(locals.i + 1);\n                if (locals.d < FEEDREG_CH_0 || locals.d - FEEDREG_CH_0 >= input.paramCount)",
     "if (locals.i + 2 > 62 || input.path.get(locals.i + 2) != FEEDREG_CH_RBRACE)\n                {\n                    return;\n                }\n                locals.d = input.path.get(locals.i + 1);\n                if (locals.d < FEEDREG_CH_0)"),
    ("            if (!locals.pathOut.empty)\n            {\n                output.returnCode = FEEDREG_ERR_FINAL_PATH;", "            if (false)\n            {\n                output.returnCode = FEEDREG_ERR_FINAL_PATH;"),
    ("input.definition.minAgeSeconds < FEEDREG_MIN_AGE_SECONDS ||", ""),
    ("|| input.definition.minAgeSeconds > FEEDREG_MAX_AGE_SECONDS)\n            {", ")\n            {"),
    ("            if (locals.pathOut.empty)\n            {\n                output.returnCode = FEEDREG_ERR_FINAL_PATH;", "            if (false)\n            {\n                output.returnCode = FEEDREG_ERR_FINAL_PATH;"),
    ("if (input.definition.decimals == 0 || input.definition.decimals > FEEDREG_MAX_DECIMALS)", "if (input.definition.decimals > FEEDREG_MAX_DECIMALS)"),
    ("            if (input.definition.decimals != 0)\n", "            if (false)\n"),
    ("if (input.definition.reserved != 0)", "if (false)"),
    ("|| input.definition.valueType == FEEDREG_VALUE_BOOL)", "|| input.definition.valueType == FEEDREG_VALUE_BOOL || input.definition.valueType == 5)"),
    # proposing
    ("if (qpi.invocationReward() < state.get().proposeDeposit)", "if (qpi.invocationReward() < state.get().proposeDeposit - 1)"),
    ("else if (state.get().feedCount >= FEEDREG_MAX_FEEDS)", "else if (false)"),
    ("if (input.feePerEpoch < 0 || input.feePerEpoch > FEEDREG_MAX_FEE_PER_EPOCH)", "if (input.feePerEpoch > FEEDREG_MAX_FEE_PER_EPOCH)"),
    ("if (state.get().liveHashes.get(output.definitionHash, locals.existing))", "if (false)"),
    ("            qpi.transfer(qpi.invocator(), qpi.invocationReward() - locals.feed.deposit);", "            qpi.transfer(qpi.invocator(), qpi.invocationReward() - locals.feed.deposit - 1);"),
    ("        state.mut().liveHashes.set(output.definitionHash, output.feedId);\n", ""),
    # withdrawing
    ("if (locals.feed.proposer != qpi.invocator())", "if (false)"),
    ("        if (locals.feed.status != FEEDREG_STATUS_PENDING)\n        {\n            output.returnCode = FEEDREG_ERR_WRONG_STATUS;", "        if (false)\n        {\n            output.returnCode = FEEDREG_ERR_WRONG_STATUS;"),
    ("output.refunded = locals.feed.deposit - locals.half;", "output.refunded = locals.half;"),
    ("        state.mut().liveHashes.removeByKey(locals.feed.definitionHash);\n        output.returnCode = FEEDREG_OK;", "        output.returnCode = FEEDREG_OK;"),
    # access
    ("            if (locals.feed.status != FEEDREG_STATUS_APPROVED)\n            {\n                output.returnCode = FEEDREG_ERR_WRONG_STATUS;", "            if (false)\n            {\n                output.returnCode = FEEDREG_ERR_WRONG_STATUS;"),
    ("locals.feed.restrictedTo != NULL_ID && locals.feed.restrictedTo != qpi.invocator()", "false"),
    ("if (input.epochs == 0 || input.epochs > FEEDREG_MAX_ACCESS_EPOCHS)", "if (input.epochs > FEEDREG_MAX_ACCESS_EPOCHS)"),
    ("else if (qpi.invocationReward() < locals.cost)", "else if (false)"),
    ("locals.until = qpi.epoch() - 1;\n", "locals.until = qpi.epoch();\n"),
    ("if (locals.until - qpi.epoch() >= FEEDREG_MAX_ACCESS_EPOCHS)", "if (false)"),
    ("            qpi.transfer(locals.feed.proposer, locals.proposerCut);", "            qpi.transfer(locals.feed.proposer, locals.proposerCut + 1);"),
    ("        state.mut().dividendPool += locals.cost - locals.proposerCut;", "        state.mut().dividendPool += locals.cost;"),
    ("        if (locals.feed.feePerEpoch == 0 && locals.feed.restrictedTo == NULL_ID)", "        if (locals.feed.feePerEpoch == 0)"),
    ("        if (locals.feed.status != FEEDREG_STATUS_APPROVED)\n        {\n            return;\n        }\n        if (locals.feed.feePerEpoch == 0", "        if (locals.feed.feePerEpoch == 0"),
    # governance
    ("locals.valid = qpi.invocationReward() >= state.get().proposalFee;", "locals.valid = true;"),
    ("&& input.data.variableOptions.variable < FEEDREG_VARIABLE_COUNT", ""),
    ("? (locals.feed.status == FEEDREG_STATUS_PENDING)", "? (true)"),
    (": (locals.feed.status == FEEDREG_STATUS_APPROVED);", ": (true);"),
    ("locals.valid = input.data.variableOptions.value >= FEEDREG_MIN_DEPOSIT && input.data.variableOptions.value <= FEEDREG_MAX_DEPOSIT;", "locals.valid = input.data.variableOptions.value <= FEEDREG_MAX_DEPOSIT;"),
    ("locals.valid = input.data.variableOptions.value <= FEEDREG_MAX_PROPOSER_SHARE_BPS;", "locals.valid = true;"),
    ("            qpi.burn(state.get().proposalFee);", "            qpi.burn(state.get().proposalFee - 1);"),
    ("if (locals.results.getAcceptedOption() != 1)", "if (locals.results.getMostVotedOption() != 1)"),
    ("if (locals.variable == FEEDREG_VAR_APPROVE_FEED && locals.feed.status == FEEDREG_STATUS_PENDING)", "if (locals.variable == FEEDREG_VAR_APPROVE_FEED)"),
    ("                        qpi.transfer(locals.feed.proposer, locals.feed.deposit);", "                        qpi.burn(locals.feed.deposit);"),
    ("                    state.mut().feeds.set(locals.value - 1, locals.feed);\n                    state.mut().liveHashes.removeByKey(locals.feed.definitionHash);", "                    state.mut().feeds.set(locals.value - 1, locals.feed);"),
    ("                state.mut().proposerShareBps = locals.value;", ""),
    ("                state.mut().proposalFee = locals.value;", ""),
    ("                state.mut().proposeDeposit = locals.value;", ""),
    ("qpi.epoch() + 1 < locals.feed.proposedEpoch + FEEDREG_PENDING_EPOCHS", "qpi.epoch() < locals.feed.proposedEpoch + FEEDREG_PENDING_EPOCHS"),
    ("qpi.epoch() + 1 < locals.feed.proposedEpoch + FEEDREG_PENDING_EPOCHS", "qpi.epoch() + 2 < locals.feed.proposedEpoch + FEEDREG_PENDING_EPOCHS"),
    ("                qpi.burn(locals.feed.deposit);\n            }\n            locals.feed.deposit = 0;\n            locals.feed.status = FEEDREG_STATUS_EXPIRED;", "            }\n            locals.feed.deposit = 0;\n            locals.feed.status = FEEDREG_STATUS_EXPIRED;"),
    ("            state.mut().dividendPool -= locals.perShare * NUMBER_OF_COMPUTORS;\n", ""),
    ("if (state.get().access.value(locals.elementIndex) <= qpi.epoch())", "if (state.get().access.value(locals.elementIndex) < qpi.epoch())"),
    # listing
    ("            if (output.count == FEEDREG_LIST_PAGE)\n            {\n                output.nextFeedId = locals.i;", "            if (output.count == FEEDREG_LIST_PAGE)\n            {\n                output.nextFeedId = locals.i + 1;"),
    ("            if (input.status != 0 && locals.feed.status != input.status)", "            if (false)"),
    ("            locals.s.restricted = (locals.feed.restrictedTo != NULL_ID) ? 1 : 0;", "            locals.s.restricted = 0;"),
]

def main():
    cmd = sys.argv[1]
    orig = open(SRC).read()
    survived = []
    for n, (a, b) in enumerate(M, 1):
        if orig.count(a) < 1:
            print(f"[{n}] pattern not found: {a[:70]!r}"); survived.append(n); continue
        work = tempfile.mkdtemp()
        open(os.path.join(work, "FeedRegistry.h"), "w").write(orig.replace(a, b, 1))
        out = os.path.join(work, "t")
        b_ = subprocess.run(cmd, shell=True, env=dict(os.environ, FEEDREG_DIR=work, OUT=out), capture_output=True, text=True)
        if b_.returncode != 0:
            shutil.rmtree(work)
            print(f"[{n}] DOES NOT COMPILE: {a.strip()[:70]!r}\n{b_.stderr[-600:]}"); survived.append(n); continue
        r = subprocess.run([out], capture_output=True, text=True)
        shutil.rmtree(work)
        caught = r.returncode != 0
        print(f"[{n}] {'caught' if caught else 'SURVIVED'}: {a.strip()[:70]!r}", flush=True)
        if not caught: survived.append(n)
    print(f"{len(M) - len(survived)}/{len(M)} mutations caught")
    sys.exit(1 if survived else 0)

main()
