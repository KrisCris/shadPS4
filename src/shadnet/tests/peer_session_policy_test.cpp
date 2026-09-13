// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// When PeerTransport gives up on a peer's session, and how it tracks begin
// requests. Time is passed in, so no case waits on a real clock.

#include <cstdio>

#include "shadnet/peer_session_policy.h"

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

using ShadNet::IsPeerSessionStale;
using ShadNet::PeerTransportState;
using ShadNet::PendingPeerBegins;
using std::chrono::seconds;

} // namespace

int main() {
    // A connected session is kept however old it is.
    CHECK(!IsPeerSessionStale(PeerTransportState::Connected, seconds{3600}));

    // A failed one is replaced at once.
    CHECK(IsPeerSessionStale(PeerTransportState::Failed, seconds{0}));
    CHECK(IsPeerSessionStale(PeerTransportState::Closed, seconds{0}));

    // The live failure: the answerer was handed a session with a peer that had
    // just logged out, and waited for an offer that never came. That has to be
    // given up on well inside the 55 s a summoning host keeps its room open.
    CHECK(!IsPeerSessionStale(PeerTransportState::WaitingForOffer, seconds{5}));
    CHECK(IsPeerSessionStale(PeerTransportState::WaitingForOffer, ShadNet::kPeerSilentDeadline));
    CHECK(ShadNet::kPeerSilentDeadline < seconds{55});
    CHECK(IsPeerSessionStale(PeerTransportState::Idle, ShadNet::kPeerSilentDeadline));

    // Checks in progress get longer than libjuice's own 39.5 s limit, so a
    // slow relayed connection is never cut off before libjuice decides.
    CHECK(!IsPeerSessionStale(PeerTransportState::Connecting, seconds{40}));
    CHECK(!IsPeerSessionStale(PeerTransportState::Gathering, seconds{40}));
    CHECK(IsPeerSessionStale(PeerTransportState::Connecting, ShadNet::kPeerConnectDeadline));

    const ShadNet::PeerClock::time_point t0{};

    // One begin per peer at a time, however often the game asks.
    {
        PendingPeerBegins begins;
        CHECK(begins.TryStart("MicBro1", t0));
        CHECK(!begins.TryStart("MicBro1", t0 + seconds{1}));
        // A different peer is independent.
        CHECK(begins.TryStart("connlost", t0 + seconds{1}));
    }

    // The session arriving clears the way for a later begin.
    {
        PendingPeerBegins begins;
        CHECK(begins.TryStart("MicBro1", t0));
        begins.SetPacketId("MicBro1", 7);
        begins.Complete("MicBro1");
        CHECK(begins.TryStart("MicBro1", t0 + seconds{1}));
    }

    // A refused begin no longer counts as in flight. Before, it stayed pending
    // forever and that peer was never asked for again.
    {
        PendingPeerBegins begins;
        CHECK(begins.TryStart("MicBro1", t0));
        begins.SetPacketId("MicBro1", 7);
        const std::optional<std::string> refused = begins.Fail(7, t0 + seconds{1});
        CHECK(refused.has_value() && *refused == "MicBro1");
        // Not on the very next tick...
        CHECK(!begins.TryStart("MicBro1", t0 + seconds{2}));
        // ...but once the retry delay has passed.
        CHECK(begins.TryStart("MicBro1", t0 + seconds{1} + PendingPeerBegins::kRetryDelay));
    }

    // A refusal for a packet nobody is waiting on changes nothing.
    {
        PendingPeerBegins begins;
        CHECK(begins.TryStart("MicBro1", t0));
        begins.SetPacketId("MicBro1", 7);
        CHECK(!begins.Fail(8, t0).has_value());
        CHECK(!begins.TryStart("MicBro1", t0 + seconds{1}));
    }

    // A begin whose reply never comes is eventually treated as lost, including
    // one whose packet id was never recorded.
    {
        PendingPeerBegins begins;
        CHECK(begins.TryStart("MicBro1", t0));
        CHECK(!begins.TryStart("MicBro1", t0 + seconds{9}));
        CHECK(begins.TryStart("MicBro1", t0 + PendingPeerBegins::kReplyTimeout));
    }

    // Detach forgets everything, back-off included.
    {
        PendingPeerBegins begins;
        CHECK(begins.TryStart("MicBro1", t0));
        begins.SetPacketId("MicBro1", 7);
        begins.Fail(7, t0);
        begins.Clear();
        CHECK(begins.TryStart("MicBro1", t0));
    }

    if (g_failures != 0) {
        std::printf("FAIL: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("PASS\n");
    return 0;
}
