#include "ut.h"

#include "bridge/ipc_dump.h"

// Scaffold: real coverage lands with the IPC protocol v2 work.

UT_TEST_CASE("ipc_dump: scaffold placeholder") {
    UT_CHECK_EQ(bridge::dump_protocol_version(""), 0);
}
