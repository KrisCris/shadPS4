// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Drives two real PeerConnections against each other through a stand-in
// signaling channel, with real libjuice agents.
//
// The channel deliberately delivers candidates BEFORE the description, which
// is the ordering the server can produce whenever the peer trickles while its
// description is still in flight. If the pending-candidate queue were wrong,
// this is where it shows.
//
// It also watches libjuice's own log for a role conflict. Two agents that both
// claim the controlling role can still reach a connection, so "they connected"
// on its own does not prove the roles were assigned correctly.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common/logging/log.h"
#include "shadnet/client.h"
#include "shadnet/peer_connection.h"

// The emulator's logging implementation pulls in Core headers that use
// clang-only attributes, so it is not linked here. The LOG_ macros look up a
// logger and do nothing when it is null, which is always the case in a test,
// so only these two symbols need to exist.
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

std::mutex g_log_mutex;
std::vector<std::string> g_role_conflicts;

// libjuice calls this from its own threads.
void OnJuiceLog(juice_log_level_t level, const char* message) {
    if (message == nullptr) {
        return;
    }
    if (std::strstr(message, "role conflict") != nullptr) {
        std::lock_guard lock(g_log_mutex);
        g_role_conflicts.emplace_back(message);
    }
    if (level >= JUICE_LOG_LEVEL_WARN) {
        std::printf("juice: %s\n", message);
    }
}

bool Check(bool condition, const char* expression, int line) {
    if (!condition) {
        std::printf("check failed at line %d: %s\n", line, expression);
        ++g_failures;
    }
    return condition;
}

#define CHECK(expression) Check((expression), #expression, __LINE__)

struct Received {
    u32 from_addr_nbo = 0;
    std::vector<u8> payload;
};

// Holds signals until Release(), so a test can choose the delivery order.
class SignalChannel {
public:
    void Send(ShadNet::PeerSignalKind kind, const std::string& payload) {
        std::lock_guard lock(m_mutex);
        if (m_held) {
            m_queue.push_back({kind, payload});
            return;
        }
        Deliver(kind, payload);
    }

    void Release() {
        std::vector<std::pair<ShadNet::PeerSignalKind, std::string>> queue;
        {
            std::lock_guard lock(m_mutex);
            m_held = false;
            queue.swap(m_queue);
        }
        // Candidates first, description last: the worst legal ordering.
        for (const auto& [kind, payload] : queue) {
            if (kind != ShadNet::PeerSignalKind::Description) {
                Deliver(kind, payload);
            }
        }
        for (const auto& [kind, payload] : queue) {
            if (kind == ShadNet::PeerSignalKind::Description) {
                Deliver(kind, payload);
            }
        }
    }

    void SetPeer(ShadNet::PeerConnection* peer) {
        std::lock_guard lock(m_mutex);
        m_peer = peer;
    }

    void SetHeld(bool held) {
        std::lock_guard lock(m_mutex);
        m_held = held;
    }

private:
    void Deliver(ShadNet::PeerSignalKind kind, const std::string& payload) {
        if (m_peer == nullptr) {
            return;
        }
        switch (kind) {
        case ShadNet::PeerSignalKind::Description:
            m_peer->OnRemoteDescription(payload);
            break;
        case ShadNet::PeerSignalKind::Candidate:
            m_peer->OnRemoteCandidate(payload);
            break;
        case ShadNet::PeerSignalKind::GatheringDone:
            m_peer->OnRemoteGatheringDone();
            break;
        }
    }

    std::mutex m_mutex;
    ShadNet::PeerConnection* m_peer = nullptr;
    bool m_held = false;
    std::vector<std::pair<ShadNet::PeerSignalKind, std::string>> m_queue;
};

bool WaitUntil(const std::function<bool()>& predicate, int timeout_seconds) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return false;
}

} // namespace

int main() {
    using ShadNet::IceServerEntry;
    using ShadNet::PeerConnection;
    using ShadNet::PeerTransportState;

    juice_set_log_handler(&OnJuiceLog);
    juice_set_log_level(JUICE_LOG_LEVEL_WARN);

    // 198.18.0.5 and 198.18.1.42 as the game would see them.
    const u32 addr_offerer = htonl(0xC6120005u);
    const u32 addr_answerer = htonl(0xC612012Au);

    std::vector<IceServerEntry> ice_servers;
    IceServerEntry stun;
    stun.host = "stun.l.google.com";
    stun.port = 19302;
    stun.is_turn = false;
    ice_servers.push_back(stun);

    SignalChannel to_answerer; // signals emitted by the offerer
    SignalChannel to_offerer;  // signals emitted by the answerer

    std::mutex received_mutex;
    std::vector<Received> offerer_received;
    std::vector<Received> answerer_received;

    PeerConnection::Params offerer_params;
    offerer_params.session_id = 42;
    offerer_params.generation = 1;
    offerer_params.is_offerer = true;
    offerer_params.peer_npid = "MintCoffeeCat";
    offerer_params.peer_virtual_addr_nbo = addr_answerer;

    PeerConnection::Params answerer_params;
    answerer_params.session_id = 42;
    answerer_params.generation = 1;
    answerer_params.is_offerer = false;
    answerer_params.peer_npid = "connlost";
    answerer_params.peer_virtual_addr_nbo = addr_offerer;

    // An answerer with no offer yet must stay silent. If it described itself
    // here it would claim the controlling role, and the offerer -- which
    // always describes itself first -- would claim it too.
    {
        std::atomic<int> idle_signals{0};
        PeerConnection::Params idle_params = answerer_params;
        idle_params.session_id = 43;
        PeerConnection idle_answerer(
            idle_params, ice_servers,
            [&idle_signals](ShadNet::PeerSignalKind, const std::string&) { ++idle_signals; },
            [](u32, const u8*, size_t) {});
        CHECK(idle_answerer.Start());
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        CHECK(idle_answerer.State() == PeerTransportState::WaitingForOffer);
        CHECK(idle_signals.load() == 0);
    }

    PeerConnection offerer(
        offerer_params, ice_servers,
        [&to_answerer](ShadNet::PeerSignalKind kind, const std::string& payload) {
            to_answerer.Send(kind, payload);
        },
        [&](u32 from, const u8* data, size_t size) {
            std::lock_guard lock(received_mutex);
            offerer_received.push_back({from, std::vector<u8>(data, data + size)});
        });

    PeerConnection answerer(
        answerer_params, ice_servers,
        [&to_offerer](ShadNet::PeerSignalKind kind, const std::string& payload) {
            to_offerer.Send(kind, payload);
        },
        [&](u32 from, const u8* data, size_t size) {
            std::lock_guard lock(received_mutex);
            answerer_received.push_back({from, std::vector<u8>(data, data + size)});
        });

    to_answerer.SetPeer(&answerer);
    to_offerer.SetPeer(&offerer);

    // Sending before a path is selected must fail rather than be silently
    // swallowed: a caller that believes a datagram went out when it did not is
    // worse off than one that sees the error.
    const u8 probe[] = {1, 2, 3, 4};
    CHECK(offerer.Send(probe, sizeof(probe)) == -1);
    CHECK(offerer.State() == PeerTransportState::Idle);

    // Hold the answerer's outbound signals so its candidates reach the offerer
    // before its description does.
    to_offerer.SetHeld(true);

    CHECK(offerer.Start());
    CHECK(answerer.Start());

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    to_offerer.Release();

    const bool connected = WaitUntil(
        [&] {
            return offerer.State() == PeerTransportState::Connected &&
                   answerer.State() == PeerTransportState::Connected;
        },
        30);
    CHECK(connected);
    if (!connected) {
        std::printf("offerer=%s answerer=%s\n", ShadNet::PeerTransportStateName(offerer.State()),
                    ShadNet::PeerTransportStateName(answerer.State()));
        std::printf("FAIL: %d check(s) failed\n", g_failures);
        return 1;
    }

    // Print the nominated pair: which family and candidate type actually won
    // is the whole point of the exercise, and it is not knowable from config.
    std::printf("offerer  path: %s\n", offerer.SelectedPath().c_str());
    std::printf("answerer path: %s\n", answerer.SelectedPath().c_str());
    std::printf("setup: offerer %lldms, answerer %lldms\n",
                static_cast<long long>(offerer.SetupMillis()),
                static_cast<long long>(answerer.SetupMillis()));
    CHECK(!offerer.SelectedPath().empty());
    CHECK(!answerer.SelectedPath().empty());
    CHECK(offerer.SetupMillis() > 0);

    // A full-size framed game datagram, not a toy payload.
    std::vector<u8> payload(1200);
    for (size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<u8>(i * 7 + 3);
    }
    CHECK(offerer.Send(payload.data(), payload.size()) == static_cast<int>(payload.size()));

    const bool delivered = WaitUntil(
        [&] {
            std::lock_guard lock(received_mutex);
            return !answerer_received.empty();
        },
        5);
    CHECK(delivered);

    if (delivered) {
        std::lock_guard lock(received_mutex);
        CHECK(answerer_received.size() == 1);
        CHECK(answerer_received[0].payload.size() == payload.size());
        CHECK(std::memcmp(answerer_received[0].payload.data(), payload.data(), payload.size()) == 0);
        // The sender's identity comes from the connection that delivered the
        // datagram, so the answerer must see the offerer's virtual address.
        CHECK(answerer_received[0].from_addr_nbo == addr_offerer);
    }

    // And back the other way, so this is not a one-directional path.
    const u8 reply[] = {'p', 'o', 'n', 'g'};
    CHECK(answerer.Send(reply, sizeof(reply)) == static_cast<int>(sizeof(reply)));
    const bool replied = WaitUntil(
        [&] {
            std::lock_guard lock(received_mutex);
            return !offerer_received.empty();
        },
        5);
    CHECK(replied);
    if (replied) {
        std::lock_guard lock(received_mutex);
        CHECK(offerer_received[0].from_addr_nbo == addr_answerer);
        CHECK(offerer_received[0].payload.size() == sizeof(reply));
    }

    // The offerer describes itself first and the answerer waits for that
    // description, so exactly one agent should ever claim the controlling role.
    {
        std::lock_guard lock(g_log_mutex);
        CHECK(g_role_conflicts.empty());
        for (const std::string& message : g_role_conflicts) {
            std::printf("unexpected: %s\n", message.c_str());
        }
    }

    if (g_failures != 0) {
        std::printf("FAIL: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("PASS\n");
    return 0;
}
