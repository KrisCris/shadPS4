// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "shadnet/peer_transport.h"

#include <optional>
#include <utility>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

#include "common/logging/log.h"
#include "shadnet/juice_log.h"
#include "shadnet/peer_connection.h"
#include "shadnet/peer_session_policy.h"

namespace ShadNet {

namespace {

// Reasons the server sends with PeerSessionClosed, and that PeerSessionEnd
// takes.
constexpr u32 kReasonCancelled = 0;
constexpr u32 kReasonConnected = 1;
constexpr u32 kReasonFailed = 2;

} // namespace

PeerTransport& PeerTransport::Instance() {
    static PeerTransport transport;
    return transport;
}

PeerTransport::PeerTransport() : m_pending_begins(std::make_unique<PendingPeerBegins>()) {}

PeerTransport::~PeerTransport() {
    Detach();
}

void PeerTransport::SetFrameHandler(FrameHandler handler) {
    std::lock_guard lock(m_mutex);
    m_on_frame = std::move(handler);
}

void PeerTransport::Attach(std::shared_ptr<ShadNetClient> client, std::string title_id) {
    if (!client) {
        Detach();
        return;
    }

    // Before any peer session exists, so libjuice's account of the first
    // connection lands in the log too.
    InstallJuiceLogForwarding();

    {
        std::lock_guard lock(m_mutex);
        m_client = client;
        m_title_id = std::move(title_id);
    }

    client->onPeerSessionOpened = [this](const NotifyPeerSessionOpened& n) {
        HandleSessionOpened(n);
    };
    client->onPeerSignal = [this](const NotifyPeerSignal& n) { HandlePeerSignal(n); };
    client->onPeerSessionClosed = [this](const NotifyPeerSessionClosed& n) {
        HandleSessionClosed(n);
    };

    // Asked for once per connection. The credentials in it are short-lived,
    // but they outlast any single session.
    const u64 pkt_id = client->GetIceServers();
    {
        std::lock_guard lock(m_mutex);
        m_ice_servers_pkt_id = pkt_id;
    }
}

void PeerTransport::Detach() {
    std::shared_ptr<ShadNetClient> client;
    std::unordered_map<u64, Session> sessions;
    {
        std::lock_guard lock(m_mutex);
        client = std::move(m_client);
        m_client.reset();
        sessions.swap(m_sessions);
        m_addresses.Clear();
        m_local_virtual_addr = 0;
        m_ice_servers.clear();
        m_ice_servers_pkt_id = 0;
        m_pending_begins->Clear();
        m_attempts.clear();
    }

    if (client) {
        client->onPeerSessionOpened = nullptr;
        client->onPeerSignal = nullptr;
        client->onPeerSessionClosed = nullptr;
    }

    // Outside the lock: each destructor joins libjuice's thread, and that
    // thread's callbacks take this same mutex.
    sessions.clear();
}

std::shared_ptr<ShadNetClient> PeerTransport::ClientHandle() const {
    std::lock_guard lock(m_mutex);
    return m_client;
}

void PeerTransport::OnReply(CommandType cmd, u64 pkt_id, ErrorType error,
                            const std::vector<u8>& body) {
    switch (cmd) {
    case CommandType::GetIceServers: {
        if (error != ErrorType::NoError) {
            LOG_WARNING(ShadNet,
                        "GetIceServers failed error={}; peers will use host candidates "
                        "only",
                        static_cast<u32>(error));
            return;
        }
        std::vector<IceServerEntry> servers = ShadNetClient::ParseIceServersReply(body);
        size_t stun_count = 0;
        size_t turn_count = 0;
        for (const IceServerEntry& server : servers) {
            (server.is_turn ? turn_count : stun_count)++;
        }
        {
            std::lock_guard lock(m_mutex);
            if (pkt_id != m_ice_servers_pkt_id) {
                return;
            }
            m_ice_servers = std::move(servers);
        }
        // Counts only: an entry's credential is a short-term secret.
        LOG_INFO(ShadNet, "ICE servers: {} stun, {} turn", stun_count, turn_count);
        return;
    }
    case CommandType::PeerSessionBegin: {
        if (error != ErrorType::NoError) {
            std::optional<std::string> npid;
            {
                std::lock_guard lock(m_mutex);
                npid = m_pending_begins->Fail(pkt_id, PeerClock::now());
            }
            if (error == ErrorType::NotFound) {
                // The server's answer while the peer is logged out. The game
                // keeps resolving, and the next begin after the retry delay
                // goes through once the peer is back.
                LOG_DEBUG(ShadNet, "peer '{}' is not online; will ask again", npid.value_or("?"));
            } else {
                LOG_WARNING(ShadNet, "PeerSessionBegin for '{}' failed error={}",
                            npid.value_or("?"), static_cast<u32>(error));
            }
            return;
        }
        NotifyPeerSessionOpened opened;
        if (!ShadNetClient::ParsePeerSessionBeginReply(body, &opened)) {
            LOG_WARNING(ShadNet, "PeerSessionBegin reply did not parse");
            return;
        }
        HandleSessionOpened(opened);
        return;
    }
    case CommandType::PeerSignal:
    case CommandType::PeerSessionEnd:
        if (error != ErrorType::NoError) {
            LOG_DEBUG(ShadNet, "peer command {} failed error={}", static_cast<u16>(cmd),
                      static_cast<u32>(error));
        }
        return;
    default:
        return;
    }
}

void PeerTransport::HandleSessionOpened(const NotifyPeerSessionOpened& notification) {
    // The single conversion point. Virtual addresses are host byte order on
    // the wire and in the server; everything from here down is network byte
    // order, because that is what the emulated socket layer hands around.
    const u32 local_addr_nbo = htonl(notification.local_virtual_addr);
    const u32 peer_addr_nbo = htonl(notification.peer_virtual_addr);

    std::vector<std::unique_ptr<PeerConnection>> superseded;
    std::optional<EndedSession> to_end;
    std::shared_ptr<ShadNetClient> client;
    PeerConnection* to_start = nullptr;
    std::vector<IceServerEntry> ice_servers;

    {
        std::lock_guard lock(m_mutex);
        client = m_client;

        const auto existing = m_sessions.find(notification.session_id);
        if (existing != m_sessions.end()) {
            if (existing->second.generation >= notification.generation) {
                // Both the reply and a notification can describe the same
                // session; the second one to arrive is not news.
                return;
            }
            // A re-ring. The old attempt's agent keeps a socket open and its
            // late packets would arrive looking legitimate, so it goes.
            superseded.push_back(DetachSessionLocked(notification.session_id));
        }

        m_pending_begins->Complete(notification.peer_npid);

        // A second session with the same peer. Both players began at once with
        // different attempt numbers -- one of them restarted, so its count
        // started over -- and the server paired each request separately. Both
        // sides are told about both sessions, so both keep the newer one, and
        // the older is ended so its attempt number is free again.
        for (const auto& [session_id, session] : m_sessions) {
            if (session.peer_npid != notification.peer_npid) {
                continue;
            }
            if (session_id > notification.session_id) {
                to_end = EndedSession{notification.session_id, notification.generation};
            } else {
                to_end = EndedSession{session_id, session.generation};
            }
            break;
        }
        if (to_end.has_value() && to_end->session_id == notification.session_id) {
            LOG_INFO(ShadNet, "peer session {} with '{}' is older than the one in use; ending it",
                     notification.session_id, notification.peer_npid);
        } else {
            if (to_end.has_value()) {
                LOG_INFO(ShadNet, "peer session {} with '{}' replaced by newer session {}",
                         to_end->session_id, notification.peer_npid, notification.session_id);
                superseded.push_back(DetachSessionLocked(to_end->session_id));
            }

            m_local_virtual_addr = local_addr_nbo;
            ice_servers = m_ice_servers;

            PeerConnection::Params params;
            params.session_id = notification.session_id;
            params.generation = notification.generation;
            params.is_offerer = notification.is_offerer;
            params.peer_npid = notification.peer_npid;
            params.peer_virtual_addr_nbo = peer_addr_nbo;

            const u64 session_id = notification.session_id;
            auto connection = std::make_unique<PeerConnection>(
                params, ice_servers,
                [this, session_id](PeerSignalKind kind, const std::string& payload) {
                    // Called from libjuice's thread, which is why nothing here
                    // may block: the mutex is held only to copy two values out.
                    std::shared_ptr<ShadNetClient> client;
                    u32 generation = 0;
                    {
                        std::lock_guard lock(m_mutex);
                        const auto it = m_sessions.find(session_id);
                        if (it == m_sessions.end() || !m_client) {
                            return;
                        }
                        generation = it->second.generation;
                        client = m_client;
                    }
                    client->PeerSignal(session_id, generation, kind, payload);
                },
                [this](u32 from_addr_nbo, const u8* data, size_t size) {
                    FrameHandler handler;
                    {
                        std::lock_guard lock(m_mutex);
                        handler = m_on_frame;
                    }
                    if (handler) {
                        handler(from_addr_nbo, data, size);
                    }
                });

            Session& session = m_sessions[notification.session_id];
            session.connection = std::move(connection);
            session.generation = notification.generation;
            session.peer_npid = notification.peer_npid;
            session.peer_addr_nbo = peer_addr_nbo;
            session.opened_at = PeerClock::now();
            to_start = session.connection.get();

            m_addresses.Insert(peer_addr_nbo, notification.peer_npid, notification.session_id);
        }
    }

    // All outside the lock: destroying joins libjuice's thread, and Start()
    // synchronously emits the offerer's description through the signal sender.
    superseded.clear();
    if (to_end.has_value() && client) {
        client->PeerSessionEnd(to_end->session_id, to_end->generation, kReasonCancelled);
    }
    if (to_start != nullptr) {
        to_start->Start();
    }
}

void PeerTransport::HandlePeerSignal(const NotifyPeerSignal& notification) {
    PeerConnection* connection = nullptr;
    {
        std::lock_guard lock(m_mutex);
        const auto it = m_sessions.find(notification.session_id);
        if (it == m_sessions.end()) {
            LOG_DEBUG(ShadNet, "signal for unknown peer session {}", notification.session_id);
            return;
        }
        if (it->second.generation != notification.generation) {
            // A straggler from a superseded attempt. Feeding it to the current
            // agent would mix two candidate sets.
            LOG_DEBUG(ShadNet, "dropping signal for session {} gen {} (current gen {})",
                      notification.session_id, notification.generation, it->second.generation);
            return;
        }
        connection = it->second.connection.get();
    }
    if (connection == nullptr) {
        return;
    }

    // Outside the lock: these run libjuice calls, which take its own locks.
    switch (notification.kind) {
    case PeerSignalKind::Description:
        connection->OnRemoteDescription(notification.payload);
        break;
    case PeerSignalKind::Candidate:
        connection->OnRemoteCandidate(notification.payload);
        break;
    case PeerSignalKind::GatheringDone:
        connection->OnRemoteGatheringDone();
        break;
    }
}

void PeerTransport::HandleSessionClosed(const NotifyPeerSessionClosed& notification) {
    {
        std::lock_guard lock(m_mutex);
        const auto it = m_sessions.find(notification.session_id);
        if (it == m_sessions.end()) {
            return;
        }
        if (it->second.generation != notification.generation) {
            return;
        }
        if (notification.reason != kReasonConnected && notification.reason != kReasonCancelled) {
            // Next resolve for this peer asks for a new session rather than
            // rejoining the one that just failed.
            ++m_attempts[it->second.peer_npid];
        }
        LOG_INFO(ShadNet, "peer session {} with '{}' closed, reason={}", notification.session_id,
                 it->second.peer_npid, notification.reason);
    }
    DropSession(notification.session_id);
}

std::unique_ptr<PeerConnection> PeerTransport::DetachSessionLocked(u64 session_id) {
    const auto it = m_sessions.find(session_id);
    if (it == m_sessions.end()) {
        return nullptr;
    }
    std::unique_ptr<PeerConnection> connection = std::move(it->second.connection);
    m_sessions.erase(it);
    m_addresses.RemoveSession(session_id);
    return connection;
}

void PeerTransport::DropSession(u64 session_id) {
    std::unique_ptr<PeerConnection> connection;
    {
        std::lock_guard lock(m_mutex);
        connection = DetachSessionLocked(session_id);
    }
    // ~PeerConnection joins libjuice's thread, whose callbacks take m_mutex.
    connection.reset();
}

int PeerTransport::SendTo(u32 dest_addr_nbo, const u8* data, size_t size) {
    if (data == nullptr || size == 0 || dest_addr_nbo == 0) {
        return -1;
    }

    PeerConnection* connection = nullptr;
    {
        std::lock_guard lock(m_mutex);
        const std::optional<u64> session_id = m_addresses.SessionFor(dest_addr_nbo);
        if (!session_id.has_value()) {
            return -1;
        }
        const auto it = m_sessions.find(*session_id);
        if (it == m_sessions.end()) {
            return -1;
        }
        connection = it->second.connection.get();
    }
    if (connection == nullptr) {
        return -1;
    }

    // No fallback to a real socket if this fails. An unresolved or
    // unconnected virtual address is an internal inconsistency, and putting an
    // RFC 2544 address on the wire either goes nowhere or goes to a stranger.
    return connection->Send(data, size);
}

u32 PeerTransport::LocalVirtualAddr() const {
    std::lock_guard lock(m_mutex);
    return m_local_virtual_addr;
}

u32 PeerTransport::ResolvePeer(std::string_view npid) {
    const std::string key(npid);
    const PeerClock::time_point now = PeerClock::now();

    std::shared_ptr<ShadNetClient> client;
    std::string title_id;
    u32 attempt = 0;
    bool begin = false;
    std::unique_ptr<PeerConnection> stale;
    std::optional<EndedSession> stale_session;
    {
        std::lock_guard lock(m_mutex);
        if (const std::optional<u32> known = m_addresses.AddrFor(key); known.has_value()) {
            const std::optional<u64> session_id = m_addresses.SessionFor(*known);
            const auto it =
                session_id.has_value() ? m_sessions.find(*session_id) : m_sessions.end();
            if (it == m_sessions.end() || !it->second.connection) {
                return *known;
            }
            const PeerTransportState state = it->second.connection->State();
            const PeerClock::duration age = now - it->second.opened_at;
            if (!IsPeerSessionStale(state, age)) {
                return *known;
            }
            // Past saving -- the peer was offline when it was made, restarted
            // since, or ICE gave up. Holding on to it would hand the game this
            // address for good, and the peer would never be asked again.
            LOG_INFO(ShadNet, "peer session {} with '{}' is {} after {}s; opening a new one",
                     it->first, key, PeerTransportStateName(state),
                     std::chrono::duration_cast<std::chrono::seconds>(age).count());
            stale_session = EndedSession{it->first, it->second.generation};
            stale = DetachSessionLocked(it->first);
            ++m_attempts[key];
        }
        client = m_client;
        if (client && m_pending_begins->TryStart(key, now)) {
            begin = true;
            title_id = m_title_id;
            attempt = m_attempts[key];
        }
    }

    // Outside the lock, for the same reasons as DropSession.
    stale.reset();
    if (!client) {
        return 0;
    }
    if (stale_session.has_value()) {
        // Tells the peer too, if it is still holding its side.
        client->PeerSessionEnd(stale_session->session_id, stale_session->generation, kReasonFailed);
    }
    if (!begin) {
        // A begin is already in flight, or waiting out a refusal.
        return 0;
    }

    LOG_DEBUG(ShadNet, "opening a peer session with '{}' (attempt {})", key, attempt);
    const u64 pkt_id = client->PeerSessionBegin(key, title_id, attempt);
    {
        std::lock_guard lock(m_mutex);
        m_pending_begins->SetPacketId(key, pkt_id);
    }
    return 0;
}

bool PeerTransport::IsPeerConnected(u32 addr_nbo) const {
    std::lock_guard lock(m_mutex);
    const std::optional<u64> session_id = m_addresses.SessionFor(addr_nbo);
    if (!session_id.has_value()) {
        return false;
    }
    const auto it = m_sessions.find(*session_id);
    if (it == m_sessions.end() || !it->second.connection) {
        return false;
    }
    return it->second.connection->State() == PeerTransportState::Connected;
}

std::string PeerTransport::SelectedPathFor(u32 addr_nbo) const {
    std::lock_guard lock(m_mutex);
    const std::optional<u64> session_id = m_addresses.SessionFor(addr_nbo);
    if (!session_id.has_value()) {
        return {};
    }
    const auto it = m_sessions.find(*session_id);
    if (it == m_sessions.end() || !it->second.connection) {
        return {};
    }
    return it->second.connection->SelectedPath();
}

} // namespace ShadNet
