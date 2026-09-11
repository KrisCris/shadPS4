// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "shadnet/peer_connection.h"

#include "common/logging/log.h"
#include "shadnet/client.h"

namespace ShadNet {

namespace {

// A peer that trickles faster than we process could otherwise queue without
// bound before its description arrives. Sixteen host/srflx/relay candidates
// across two families is already generous; past that, something is wrong.
constexpr size_t kMaxPendingCandidates = 64;

PeerTransportState FromJuiceState(juice_state_t state) {
    switch (state) {
    case JUICE_STATE_GATHERING:
        return PeerTransportState::Gathering;
    case JUICE_STATE_CONNECTING:
        return PeerTransportState::Connecting;
    case JUICE_STATE_CONNECTED:
    case JUICE_STATE_COMPLETED:
        return PeerTransportState::Connected;
    case JUICE_STATE_FAILED:
        return PeerTransportState::Failed;
    case JUICE_STATE_DISCONNECTED:
    default:
        return PeerTransportState::Idle;
    }
}

} // namespace

const char* PeerTransportStateName(PeerTransportState state) {
    switch (state) {
    case PeerTransportState::Idle:
        return "idle";
    case PeerTransportState::WaitingForOffer:
        return "waiting-for-offer";
    case PeerTransportState::Gathering:
        return "gathering";
    case PeerTransportState::Connecting:
        return "connecting";
    case PeerTransportState::Connected:
        return "connected";
    case PeerTransportState::Failed:
        return "failed";
    case PeerTransportState::Closed:
        return "closed";
    }
    return "unknown";
}

PeerConnection::PeerConnection(Params params, const std::vector<IceServerEntry>& ice_servers,
                               SignalSender send_signal, ReceiveHandler on_receive)
    : m_params(std::move(params)), m_send_signal(std::move(send_signal)),
      m_on_receive(std::move(on_receive)) {
    // juice_config_t holds raw pointers that the agent reads for its whole
    // life, so every string it will see has to outlive the agent here.
    for (const IceServerEntry& server : ice_servers) {
        if (server.host.empty()) {
            continue;
        }
        if (server.is_turn) {
            m_turn_servers.push_back(
                TurnServer{server.host, server.username, server.credential, server.port});
        } else if (m_stun_host.empty()) {
            m_stun_host = server.host;
            m_stun_port = server.port;
        }
    }
}

PeerConnection::~PeerConnection() {
    juice_agent_t* agent = nullptr;
    {
        std::lock_guard lock(m_mutex);
        agent = m_agent;
        m_agent = nullptr;
    }
    m_state.store(PeerTransportState::Closed);
    if (agent != nullptr) {
        // Blocks until libjuice's thread is done, so no callback can be in
        // flight against a half-destroyed object after this returns.
        juice_destroy(agent);
    }
}

bool PeerConnection::Start() {
    std::vector<juice_turn_server_t> turn_servers;
    turn_servers.reserve(m_turn_servers.size());
    for (const TurnServer& server : m_turn_servers) {
        juice_turn_server_t entry{};
        entry.host = server.host.c_str();
        entry.username = server.username.c_str();
        entry.password = server.password.c_str();
        entry.port = server.port;
        turn_servers.push_back(entry);
    }

    juice_config_t config{};
    config.concurrency_mode = JUICE_CONCURRENCY_MODE_POLL;
    if (!m_stun_host.empty()) {
        config.stun_server_host = m_stun_host.c_str();
        config.stun_server_port = m_stun_port;
    }
    if (!turn_servers.empty()) {
        // Relay gathering runs alongside the direct checks, so falling back
        // does not mean first waiting out a full direct failure.
        config.turn_servers = turn_servers.data();
        config.turn_servers_count = static_cast<int>(turn_servers.size());
    }
    config.cb_state_changed = &PeerConnection::OnStateChangedThunk;
    config.cb_candidate = &PeerConnection::OnCandidateThunk;
    config.cb_gathering_done = &PeerConnection::OnGatheringDoneThunk;
    config.cb_recv = &PeerConnection::OnRecvThunk;
    config.user_ptr = this;

    juice_agent_t* agent = juice_create(&config);
    if (agent == nullptr) {
        LOG_ERROR(ShadNet, "Peer session {}: could not create ICE agent for '{}'",
                  m_params.session_id, m_params.peer_npid);
        SetState(PeerTransportState::Failed);
        return false;
    }

    std::string stashed_description;
    bool have_stashed_description = false;
    {
        std::lock_guard lock(m_mutex);
        m_agent = agent;
        m_started_at = std::chrono::steady_clock::now();
        have_stashed_description = m_has_stashed_remote_description;
        stashed_description.swap(m_stashed_remote_description);
        m_has_stashed_remote_description = false;
    }

    LOG_INFO(ShadNet, "Peer session {} gen {}: starting ICE with '{}' as {} (stun={} turn={})",
             m_params.session_id, m_params.generation, m_params.peer_npid,
             m_params.is_offerer ? "offerer" : "answerer",
             m_stun_host.empty() ? "none" : m_stun_host, m_turn_servers.size());

    if (m_params.is_offerer) {
        return StartLocalSide(agent);
    }

    // The answerer must not describe itself first; see the note on Start() in
    // the header. If the offer beat this call, apply it now -- otherwise wait.
    if (have_stashed_description) {
        ApplyRemoteDescription(stashed_description);
        return true;
    }
    SetState(PeerTransportState::WaitingForOffer);
    return true;
}

bool PeerConnection::StartLocalSide(juice_agent_t* agent) {
    char sdp[JUICE_MAX_SDP_STRING_LEN]{};
    if (juice_get_local_description(agent, sdp, sizeof(sdp)) < 0) {
        LOG_ERROR(ShadNet, "Peer session {}: could not read local ICE description",
                  m_params.session_id);
        SetState(PeerTransportState::Failed);
        return false;
    }

    {
        std::lock_guard lock(m_mutex);
        m_local_side_started = true;
    }
    SetState(PeerTransportState::Gathering);

    if (m_send_signal) {
        m_send_signal(PeerSignalKind::Description, std::string(sdp));
    }
    juice_gather_candidates(agent);
    return true;
}

void PeerConnection::OnRemoteDescription(const std::string& sdp) {
    {
        std::lock_guard lock(m_mutex);
        if (m_remote_description_set) {
            // A duplicate is normal: both sides may resend after a retry.
            return;
        }
        if (m_agent == nullptr) {
            // Start() has not run yet. Hold it rather than drop it: for the
            // answerer this message is what starts everything.
            m_stashed_remote_description = sdp;
            m_has_stashed_remote_description = true;
            return;
        }
    }
    ApplyRemoteDescription(sdp);
}

void PeerConnection::ApplyRemoteDescription(const std::string& sdp) {
    std::vector<std::string> to_flush;
    bool flush_gathering_done = false;
    bool describe_local_side = false;
    juice_agent_t* agent = nullptr;

    {
        std::lock_guard lock(m_mutex);
        if (m_agent == nullptr || m_remote_description_set) {
            return;
        }
        agent = m_agent;
        m_remote_description_set = true;
        to_flush.swap(m_pending_candidates);
        flush_gathering_done = m_remote_gathering_done_pending;
        m_remote_gathering_done_pending = false;
        describe_local_side = !m_local_side_started;
    }

    juice_set_remote_description(agent, sdp.c_str());

    // Strictly after the call above, never before it: that ordering is what
    // gives the answerer the controlled role.
    if (describe_local_side && !StartLocalSide(agent)) {
        return;
    }

    for (const std::string& candidate : to_flush) {
        juice_add_remote_candidate(agent, candidate.c_str());
    }
    if (flush_gathering_done) {
        juice_set_remote_gathering_done(agent);
    }
    if (!to_flush.empty()) {
        LOG_DEBUG(ShadNet, "Peer session {}: flushed {} candidate(s) held before the description",
                  m_params.session_id, to_flush.size());
    }
}

void PeerConnection::OnRemoteCandidate(const std::string& sdp) {
    juice_agent_t* agent = nullptr;
    {
        std::lock_guard lock(m_mutex);
        if (!m_remote_description_set) {
            // The peer trickles as it gathers, so a candidate can beat the
            // description. Dropping it could discard the only working path.
            if (m_pending_candidates.size() >= kMaxPendingCandidates) {
                LOG_DEBUG(ShadNet, "Peer session {}: dropping candidate, {} already held",
                          m_params.session_id, m_pending_candidates.size());
                return;
            }
            m_pending_candidates.push_back(sdp);
            return;
        }
        if (m_agent == nullptr) {
            return;
        }
        agent = m_agent;
    }
    juice_add_remote_candidate(agent, sdp.c_str());
}

void PeerConnection::OnRemoteGatheringDone() {
    juice_agent_t* agent = nullptr;
    {
        std::lock_guard lock(m_mutex);
        if (!m_remote_description_set) {
            m_remote_gathering_done_pending = true;
            return;
        }
        if (m_agent == nullptr) {
            return;
        }
        agent = m_agent;
    }
    juice_set_remote_gathering_done(agent);
}

int PeerConnection::Send(const u8* data, size_t size) {
    if (data == nullptr || size == 0) {
        return -1;
    }
    if (m_state.load() != PeerTransportState::Connected) {
        return -1;
    }

    juice_agent_t* agent = nullptr;
    {
        std::lock_guard lock(m_mutex);
        agent = m_agent;
    }
    if (agent == nullptr) {
        return -1;
    }

    const int rc = juice_send(agent, reinterpret_cast<const char*>(data), size);
    if (rc < 0) {
        LOG_DEBUG(ShadNet, "Peer session {}: send of {} bytes failed rc={}", m_params.session_id,
                  size, rc);
        return -1;
    }
    return static_cast<int>(size);
}

PeerTransportState PeerConnection::State() const {
    return m_state.load();
}

u64 PeerConnection::SessionId() const {
    return m_params.session_id;
}

u32 PeerConnection::Generation() const {
    return m_params.generation;
}

const std::string& PeerConnection::PeerNpid() const {
    return m_params.peer_npid;
}

u32 PeerConnection::PeerVirtualAddrNbo() const {
    return m_params.peer_virtual_addr_nbo;
}

std::string PeerConnection::SelectedPath() const {
    std::lock_guard lock(m_mutex);
    return m_selected_path;
}

s64 PeerConnection::SetupMillis() const {
    return m_setup_ms.load();
}

void PeerConnection::SetState(PeerTransportState state) {
    m_state.store(state);
}

void PeerConnection::OnStateChangedThunk(juice_agent_t*, juice_state_t state, void* user) {
    static_cast<PeerConnection*>(user)->HandleStateChanged(state);
}

void PeerConnection::OnCandidateThunk(juice_agent_t*, const char* sdp, void* user) {
    static_cast<PeerConnection*>(user)->HandleCandidate(sdp);
}

void PeerConnection::OnGatheringDoneThunk(juice_agent_t*, void* user) {
    static_cast<PeerConnection*>(user)->HandleGatheringDone();
}

void PeerConnection::OnRecvThunk(juice_agent_t*, const char* data, size_t size, void* user) {
    static_cast<PeerConnection*>(user)->HandleReceive(data, size);
}

void PeerConnection::HandleStateChanged(juice_state_t state) {
    const PeerTransportState mapped = FromJuiceState(state);
    const PeerTransportState previous = m_state.exchange(mapped);
    if (previous == mapped) {
        return;
    }

    if (mapped == PeerTransportState::Connected && previous != PeerTransportState::Connected) {
        char local[JUICE_MAX_CANDIDATE_SDP_STRING_LEN]{};
        char remote[JUICE_MAX_CANDIDATE_SDP_STRING_LEN]{};
        std::string path;
        s64 elapsed_ms = 0;
        {
            std::lock_guard lock(m_mutex);
            if (m_agent != nullptr && juice_get_selected_candidates(m_agent, local, sizeof(local),
                                                                    remote, sizeof(remote)) >= 0) {
                m_selected_path = std::string(local) + " | " + remote;
                path = m_selected_path;
            }
            elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - m_started_at)
                             .count();
        }
        m_setup_ms.store(elapsed_ms);
        // The nominated pair is the only evidence of which path and family
        // actually won; a configured address proves nothing.
        LOG_INFO(ShadNet, "Peer session {} connected to '{}' in {}ms via {}", m_params.session_id,
                 m_params.peer_npid, elapsed_ms, path.empty() ? "unknown path" : path);
        return;
    }

    if (mapped == PeerTransportState::Failed) {
        LOG_WARNING(ShadNet, "Peer session {}: ICE failed with '{}' (was {})", m_params.session_id,
                    m_params.peer_npid, PeerTransportStateName(previous));
        return;
    }

    LOG_DEBUG(ShadNet, "Peer session {}: {} -> {}", m_params.session_id,
              PeerTransportStateName(previous), PeerTransportStateName(mapped));
}

void PeerConnection::HandleCandidate(const char* sdp) {
    if (sdp == nullptr) {
        return;
    }
    LOG_DEBUG(ShadNet, "Peer session {}: local candidate {}", m_params.session_id, sdp);
    if (m_send_signal) {
        // Trickled as gathered rather than batched at the end, so checks start
        // against the first candidates while the rest are still being found.
        m_send_signal(PeerSignalKind::Candidate, std::string(sdp));
    }
}

void PeerConnection::HandleGatheringDone() {
    LOG_DEBUG(ShadNet, "Peer session {}: local gathering done", m_params.session_id);
    if (m_send_signal) {
        m_send_signal(PeerSignalKind::GatheringDone, std::string());
    }
}

void PeerConnection::HandleReceive(const char* data, size_t size) {
    if (data == nullptr || size == 0 || !m_on_receive) {
        return;
    }
    // The sender's identity comes from the connection that delivered the
    // datagram, never from anything inside it.
    m_on_receive(m_params.peer_virtual_addr_nbo, reinterpret_cast<const u8*>(data), size);
}

} // namespace ShadNet
