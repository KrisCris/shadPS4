// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <unordered_map>

#include "common/types.h"
#include "shadnet/peer_connection.h"

// The decisions PeerTransport makes about a peer's session, kept apart from
// the transport so they can be tested without a server connection or a clock
// that has to be waited on.

namespace ShadNet {

using PeerClock = std::chrono::steady_clock;

// How long a session may sit unconnected before resolving the peer replaces it.
//
// No answer at all: the offerer's description follows its session within a
// round trip, so an answerer still waiting after this is waiting on a peer
// that is not there. libjuice never fails that case on its own, because an
// agent with no remote description has nothing to check.
inline constexpr std::chrono::seconds kPeerSilentDeadline{15};
// Checks under way: libjuice gives up on its own after 39.5 s (ICE_PAC_TIMEOUT
// in agent.h), so this only catches an agent that never got that far.
inline constexpr std::chrono::seconds kPeerConnectDeadline{45};

// Whether a session is past saving, so the peer should get a new one. A
// session that was handed an offline peer, or whose peer restarted, otherwise
// holds the peer's address forever and every later resolve returns it.
bool IsPeerSessionStale(PeerTransportState state, PeerClock::duration age);

// Begin requests in flight, one per peer.
//
// A game resolves a peer on a timer, so without this every tick would open a
// session. And the one thing a begin must not do is stay "in flight" after the
// server refused it -- the peer was then never asked for again.
//
// Not thread-safe; PeerTransport holds its own lock around every call.
class PendingPeerBegins {
public:
    // No reply for this long and the begin is treated as lost.
    static constexpr std::chrono::seconds kReplyTimeout{10};
    // After a refusal -- typically the peer is offline -- wait this long before
    // asking again, instead of on every tick of the game's timer.
    static constexpr std::chrono::seconds kRetryDelay{2};

    // True when a begin for npid may be sent now, in which case it is recorded
    // as in flight.
    bool TryStart(const std::string& npid, PeerClock::time_point now);

    // Ties the begin just started to its request, so a refusal can be traced
    // back to the peer it was for.
    void SetPacketId(const std::string& npid, u64 pkt_id);

    // A session for npid arrived, by reply or notification.
    void Complete(const std::string& npid);

    // The server refused a begin. Returns the peer it was for, or nothing if
    // no begin in flight has that packet id.
    std::optional<std::string> Fail(u64 pkt_id, PeerClock::time_point now);

    void Clear();

private:
    struct InFlight {
        u64 pkt_id = 0;
        PeerClock::time_point sent_at{};
    };

    std::unordered_map<std::string, InFlight> m_in_flight;
    std::unordered_map<std::string, PeerClock::time_point> m_retry_after;
};

} // namespace ShadNet
