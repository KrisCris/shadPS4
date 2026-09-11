// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include <juice/juice.h>

#include "common/types.h"

// Only src/shadnet may include libjuice; see CONTRIBUTING.md. This header is
// consumed inside that module, never from Core.

namespace ShadNet {

enum class PeerSignalKind : u32;
struct IceServerEntry;

enum class PeerTransportState {
    Idle,
    // Answerer only: the agent exists but has deliberately not produced its
    // local description yet. See the ICE role note on Start().
    WaitingForOffer,
    Gathering,
    Connecting,
    Connected,
    Failed,
    Closed,
};

const char* PeerTransportStateName(PeerTransportState state);

// One ICE agent for one peer.
//
// The agent owns the established NAT mapping and any relay allocation, so it
// stays alive for the whole connection and every datagram continues to go
// through it. Discovering an address with ICE and then sending from a
// different socket would throw that state away -- it happens to work on a LAN
// and fails everywhere else, which makes it an expensive mistake to make.
class PeerConnection {
public:
    // Called with a framed datagram received from this peer. from_addr_nbo is
    // the peer's virtual address, never a physical one, so callers upstream
    // cannot accidentally start routing on it.
    using ReceiveHandler = std::function<void(u32 from_addr_nbo, const u8* data, size_t size)>;

    // Sends one signaling message to the peer through the server. Must not
    // block: it is called from libjuice's own thread.
    using SignalSender = std::function<void(PeerSignalKind kind, const std::string& payload)>;

    struct Params {
        u64 session_id = 0;
        u32 generation = 0;
        bool is_offerer = false;
        std::string peer_npid;
        u32 peer_virtual_addr_nbo = 0;
    };

    PeerConnection(Params params, const std::vector<IceServerEntry>& ice_servers,
                   SignalSender send_signal, ReceiveHandler on_receive);
    ~PeerConnection();

    PeerConnection(const PeerConnection&) = delete;
    PeerConnection& operator=(const PeerConnection&) = delete;

    // Creates the agent. The offerer immediately emits its local description
    // and begins gathering; the answerer waits for the offer first.
    //
    // That asymmetry is load-bearing, not a nicety. libjuice has no way to set
    // the ICE role explicitly: whichever call comes first decides it, and
    // juice_get_local_description() claims the controlling role when the role
    // is still unknown. If both sides described themselves first, both would be
    // controlling, and the role-conflict repair in RFC 8445 7.3.1.1 would have
    // to sort it out by tiebreaker -- which in practice leaves one side stuck
    // in checking. The server already decided who offers, so use that.
    bool Start();

    void OnRemoteDescription(const std::string& sdp);
    void OnRemoteCandidate(const std::string& sdp);
    void OnRemoteGatheringDone();

    // Returns bytes accepted, or -1. Fails rather than buffering while the
    // path is not yet selected: a caller that believes a datagram was sent
    // when it was dropped is worse than one that sees the failure.
    int Send(const u8* data, size_t size);

    PeerTransportState State() const;
    u64 SessionId() const;
    u32 Generation() const;
    const std::string& PeerNpid() const;
    u32 PeerVirtualAddrNbo() const;

    // "<local candidate> | <remote candidate>", empty until connected. Log it:
    // a configured IPv6 address is not evidence of an IPv6 connection, only
    // the nominated pair is.
    std::string SelectedPath() const;

    // Milliseconds from Start() to reaching Connected, 0 if not connected yet.
    s64 SetupMillis() const;

private:
    // libjuice invokes these on its own thread.
    static void OnStateChangedThunk(juice_agent_t* agent, juice_state_t state, void* user);
    static void OnCandidateThunk(juice_agent_t* agent, const char* sdp, void* user);
    static void OnGatheringDoneThunk(juice_agent_t* agent, void* user);
    static void OnRecvThunk(juice_agent_t* agent, const char* data, size_t size, void* user);

    void HandleStateChanged(juice_state_t state);
    void HandleCandidate(const char* sdp);
    void HandleGatheringDone();
    void HandleReceive(const char* data, size_t size);

    // Emits the local description and starts gathering. Called without the
    // lock held, because it runs libjuice calls and the signal sender.
    bool StartLocalSide(juice_agent_t* agent);

    // Hands the description to the agent and, for the answerer, describes the
    // local side afterwards. Requires the agent to exist.
    void ApplyRemoteDescription(const std::string& sdp);

    void SetState(PeerTransportState state);

    const Params m_params;
    const SignalSender m_send_signal;
    const ReceiveHandler m_on_receive;

    // Copies, because juice_config_t holds raw pointers into whatever the
    // caller passed and the agent reads them for its whole life.
    struct TurnServer {
        std::string host;
        std::string username;
        std::string password;
        u16 port = 0;
    };
    std::string m_stun_host;
    u16 m_stun_port = 0;
    std::vector<TurnServer> m_turn_servers;

    mutable std::mutex m_mutex;
    juice_agent_t* m_agent = nullptr;
    std::atomic<PeerTransportState> m_state{PeerTransportState::Idle};
    bool m_local_side_started = false;
    bool m_remote_description_set = false;
    // The offer can arrive before Start() runs, since the notification that
    // creates this object and the first signal travel the same connection.
    // Holding it means the answerer does not deadlock waiting for a message
    // that already came and went.
    std::string m_stashed_remote_description;
    bool m_has_stashed_remote_description = false;
    // Candidates can arrive before the remote description, since the peer
    // trickles as it gathers and the two messages race. Hold them rather than
    // discarding a candidate that may be the only working one.
    std::vector<std::string> m_pending_candidates;
    bool m_remote_gathering_done_pending = false;
    std::string m_selected_path;
    std::chrono::steady_clock::time_point m_started_at{};
    std::atomic<s64> m_setup_ms{0};
};

} // namespace ShadNet
