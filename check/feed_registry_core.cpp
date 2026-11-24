// Syntax and type check of FeedRegistry.h against the real QPI headers of qubic/core.
// Core's contract build needs MSVC; this checks only what a Linux compiler can see:
// every QPI call, type and macro the contract uses exists with a matching signature,
// and the sizes below. Proposal-voting internals are stubbed (their real definition
// pulls in the UEFI platform layer).
#define NO_UEFI
#include "contract_core/pre_qpi_def.h"
#include "qpi/qpi.h"
namespace QPI
{
    template <uint16 proposalSlotCount, uint64 contractAssetName>
    struct ProposalAndVotingByShareholders
    {
        static constexpr uint16 maxProposals = proposalSlotCount;
        static constexpr uint32 maxVotes = NUMBER_OF_COMPUTORS;
    };
    template <typename ProposalDataType, uint32 numOfVoters>
    struct ProposalWithAllVoteData { ProposalDataType proposalData; };
}
#define CONTRACT_INDEX 63
#define CONTRACT_STATE_TYPE FEEDREG
#define CONTRACT_STATE2_TYPE FEEDREG2
#include "FeedRegistry.h"
#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE

static_assert(sizeof(FEEDREG::FeedDefinition) == 396, "FeedDefinition layout changed: definition hashes change too");
static_assert(sizeof(FEEDREG::ProposeFeed_input) <= MAX_INPUT_SIZE, "ProposeFeed input too large for a transaction");
static_assert(sizeof(FEEDREG::CheckDefinition_input) <= MAX_INPUT_SIZE, "CheckDefinition input too large for a request");
int main() { return 0; }
