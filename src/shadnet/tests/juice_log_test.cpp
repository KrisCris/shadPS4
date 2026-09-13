// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// libjuice's log lines pass through RedactJuiceLogMessage on their way into the
// emulator log. The inputs are libjuice's own formats (agent.c), filled in with
// made-up credentials.

#include <cstdio>
#include <string>
#include <string_view>

#include "common/logging/log.h"
#include "shadnet/juice_log.h"

// Same logging stub as peer_connection_test: LOG_ looks up a logger and does
// nothing when it is null, so only these two symbols need to exist.
namespace Common {
namespace Log {
std::unordered_map<std::string_view, std::shared_ptr<spdlog::logger>> ALL_LOGGERS;
} // namespace Log
std::string GetCurrentThreadName() {
    return "test";
}
} // namespace Common

namespace {

int g_failures = 0;

bool Check(bool condition, const char* expression, int line) {
    if (!condition) {
        std::printf("check failed at line %d: %s\n", line, expression);
        ++g_failures;
    }
    return condition;
}

#define CHECK(expression) Check((expression), #expression, __LINE__)

bool Hides(std::string_view message, std::string_view secret) {
    return ShadNet::RedactJuiceLogMessage(message).find(secret) == std::string::npos;
}

bool Unchanged(std::string_view message) {
    return ShadNet::RedactJuiceLogMessage(message) == message;
}

} // namespace

int main() {
    using ShadNet::RedactJuiceLogMessage;

    // Credentials libjuice quotes when a check fails are cut out.
    const std::string_view password = R"(STUN integrity check failed, password="Xq7pLm2Rz9")";
    CHECK(Hides(password, "Xq7pLm2Rz9"));
    CHECK(RedactJuiceLogMessage(password) == "STUN integrity check failed, password=<redacted>");

    const std::string_view local_ufrag =
        R"(STUN local ufrag check failed, expected="aB3d", actual="zY9w")";
    CHECK(Hides(local_ufrag, "aB3d"));
    CHECK(Hides(local_ufrag, "zY9w"));

    const std::string_view remote_ufrag =
        R"(STUN remote ufrag check failed, expected="aB3d", actual="zY9w")";
    CHECK(Hides(remote_ufrag, "aB3d"));
    CHECK(Hides(remote_ufrag, "zY9w"));

    CHECK(Hides(R"(STUN username invalid, username="aB3d:zY9w")", "aB3d:zY9w"));

    // A line that names a credential without quoting one stays whole.
    CHECK(Unchanged("STUN integrity check failed, unknown password"));
    CHECK(Unchanged("Missing ICE password in remote description"));

    // Ordinary diagnostics, quoted or not, are untouched: they are the point.
    CHECK(Unchanged("STUN entry 3: Consent expired for candidate pair"));
    CHECK(Unchanged("TURN allocation failed"));
    CHECK(Unchanged("Received a datagram from unknown address, ignoring"));
    CHECK(Unchanged(R"(Got STUN error code 403, reason "Forbidden IP")"));
    CHECK(Unchanged(""));

    if (g_failures != 0) {
        std::printf("FAIL: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("PASS\n");
    return 0;
}
