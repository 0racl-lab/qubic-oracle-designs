// Compiles the interface headers against qubic/core's own QPI headers, the same way
// src/oracle_core/oracle_interfaces_def.h includes real interfaces, and checks their layouts.
// Build: see check/check.sh (needs a qubic/core checkout; the script clones one if needed).
// the same headers core makes available to contracts and oracle interfaces
#include "contract_core/pre_qpi_def.h"
#include "qpi/qpi.h"
#include "oracle_core/oracle_interfaces_def.h"   // the five real interfaces (indices 0-4)
#include "network_messages/common_def.h"

namespace OI
{
#define ORACLE_INTERFACE_INDEX 5
#include "ContentRead.h"
#undef ORACLE_INTERFACE_INDEX
#define ORACLE_INTERFACE_INDEX 6
#include "UtxoTxRead.h"
#undef ORACLE_INTERFACE_INDEX
#define ORACLE_INTERFACE_INDEX 7
#include "EvmCallRead.h"
#undef ORACLE_INTERFACE_INDEX
#define ORACLE_INTERFACE_INDEX 8
#include "HttpRead.h"
#undef ORACLE_INTERFACE_INDEX
}

#include <cstddef>
#include <cstdio>

#define FITS(I) \
    static_assert(sizeof(OI::I::OracleQuery) <= MAX_ORACLE_QUERY_SIZE, #I " query too large"); \
    static_assert(sizeof(OI::I::OracleReply) <= MAX_ORACLE_REPLY_SIZE, #I " reply too large")
FITS(ContentRead); FITS(UtxoTxRead); FITS(EvmCallRead); FITS(HttpRead);

// exact sizes and offsets documented in the headers (no hidden padding)
static_assert(sizeof(OI::ContentRead::OracleQuery) == 208 && sizeof(OI::ContentRead::OracleReply) == 536);
static_assert(offsetof(OI::ContentRead::OracleQuery, maxFileSize) == 112 && offsetof(OI::ContentRead::OracleQuery, jsonPath) == 144);
static_assert(offsetof(OI::ContentRead::OracleReply, data) == 24);
static_assert(sizeof(OI::UtxoTxRead::OracleQuery) == 56 && sizeof(OI::UtxoTxRead::OracleReply) == 208);
static_assert(offsetof(OI::UtxoTxRead::OracleReply, value) == 64 && offsetof(OI::UtxoTxRead::OracleReply, script) == 80);
static_assert(sizeof(OI::EvmCallRead::OracleQuery) == 568 && sizeof(OI::EvmCallRead::OracleReply) == 568);
static_assert(offsetof(OI::EvmCallRead::OracleQuery, calldata) == 56 && offsetof(OI::EvmCallRead::OracleReply, data) == 56);
static_assert(sizeof(OI::HttpRead::OracleQuery) == 256 && sizeof(OI::HttpRead::OracleReply) == 160);
static_assert(offsetof(OI::HttpRead::OracleQuery, timestamp) == 40 && offsetof(OI::HttpRead::OracleQuery, params) == 48);
static_assert(offsetof(OI::HttpRead::OracleQuery, field) == 176 && offsetof(OI::HttpRead::OracleQuery, decimals) == 248);
static_assert(offsetof(OI::HttpRead::OracleReply, intValue) == 16 && offsetof(OI::HttpRead::OracleReply, text) == 32);

int main()
{
    using QPI::sint64;
    // fees: every query must cost at least core's 10 QU minimum
    OI::ContentRead::OracleQuery c{}; c.maxFileSize = 1; sint64 f1 = OI::ContentRead::getQueryFee(c);
    c.maxFileSize = OI::ContentRead::MAX_FILE_SIZE; sint64 f2 = OI::ContentRead::getQueryFee(c);
    printf("ContentRead  query %4zu  reply %4zu  fee %lld..%lld QU\n", sizeof(OI::ContentRead::OracleQuery), sizeof(OI::ContentRead::OracleReply), (long long)f1, (long long)f2);
    printf("UtxoTxRead   query %4zu  reply %4zu  fee %lld QU\n", sizeof(OI::UtxoTxRead::OracleQuery), sizeof(OI::UtxoTxRead::OracleReply), (long long)OI::UtxoTxRead::getQueryFee({}));
    printf("EvmCallRead  query %4zu  reply %4zu  fee %lld QU\n", sizeof(OI::EvmCallRead::OracleQuery), sizeof(OI::EvmCallRead::OracleReply), (long long)OI::EvmCallRead::getQueryFee({}));
    printf("HttpRead     query %4zu  reply %4zu  fee %lld QU\n", sizeof(OI::HttpRead::OracleQuery), sizeof(OI::HttpRead::OracleReply), (long long)OI::HttpRead::getQueryFee({}));
    printf("limits: query <= %d, reply <= %d bytes\n", (int)MAX_ORACLE_QUERY_SIZE, (int)MAX_ORACLE_REPLY_SIZE);
    return (f1 >= 10 && f2 >= f1) ? 0 : 1;
}
