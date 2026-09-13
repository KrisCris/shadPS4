// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "shadnet/peer_session_policy.h"

namespace ShadNet {

bool IsPeerSessionStale(PeerTransportState state, PeerClock::duration age) {
    switch (state) {
    case PeerTransportState::Connected:
        return false;
    case PeerTransportState::Failed:
    case PeerTransportState::Closed:
        return true;
    case PeerTransportState::Idle:
    case PeerTransportState::WaitingForOffer:
        return age >= kPeerSilentDeadline;
    case PeerTransportState::Gathering:
    case PeerTransportState::Connecting:
        return age >= kPeerConnectDeadline;
    }
    return false;
}

bool PendingPeerBegins::TryStart(const std::string& npid, PeerClock::time_point now) {
    if (const auto retry = m_retry_after.find(npid); retry != m_retry_after.end()) {
        if (now < retry->second) {
            return false;
        }
        m_retry_after.erase(retry);
    }
    if (const auto in_flight = m_in_flight.find(npid); in_flight != m_in_flight.end()) {
        if (now - in_flight->second.sent_at < kReplyTimeout) {
            return false;
        }
    }
    m_in_flight[npid] = InFlight{0, now};
    return true;
}

void PendingPeerBegins::SetPacketId(const std::string& npid, u64 pkt_id) {
    const auto it = m_in_flight.find(npid);
    if (it != m_in_flight.end()) {
        it->second.pkt_id = pkt_id;
    }
}

void PendingPeerBegins::Complete(const std::string& npid) {
    m_in_flight.erase(npid);
}

std::optional<std::string> PendingPeerBegins::Fail(u64 pkt_id, PeerClock::time_point now) {
    for (auto it = m_in_flight.begin(); it != m_in_flight.end(); ++it) {
        if (it->second.pkt_id != pkt_id) {
            continue;
        }
        std::string npid = it->first;
        m_in_flight.erase(it);
        m_retry_after[npid] = now + kRetryDelay;
        return npid;
    }
    return std::nullopt;
}

void PendingPeerBegins::Clear() {
    m_in_flight.clear();
    m_retry_after.clear();
}

} // namespace ShadNet
