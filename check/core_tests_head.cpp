#define NO_UEFI
// Linux build of the EvmCallRead / UtxoTxRead tests from test/oracle_engine.cpp (core's full test
// suite builds with MSVC only). Only the interface registry and QPI are needed for these tests.
#include "gtest/gtest.h"
#include "contract_core/pre_qpi_def.h"
#include "qpi/qpi.h"
#include "oracle_core/oracle_interfaces_def.h"
#include "network_messages/common_def.h"
#include "platform/memory.h"
#include <cstddef>
constexpr int64_t MIN_ORACLE_QUERY_FEE = 10;   // from oracle_core/oracle_engine.h
#include <cstring>
// NO_UEFI implementations, as in core's test/stdlib_impl.cpp
void setMem(void* buffer, unsigned long long size, unsigned char value) { memset(buffer, value, size); }
void copyMem(void* destination, const void* source, unsigned long long length) { memcpy(destination, source, length); }
