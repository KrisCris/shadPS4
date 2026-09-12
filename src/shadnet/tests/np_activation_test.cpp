// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Exercise the production activation queue with asynchronous peer discovery.
// Transport and kernel entry points are replaced so no game process is needed.
#include <cstdio>
#include <functional>
#include <mutex>
#include <vector>

#include "common/logging/log.h"
#include "core/libraries/network/net.h"
#include "core/libraries/np/np_signaling/np_signaling_state.h"
#include "core/libraries/np/np_signaling/np_signaling_stubs.h"

namespace Common {
namespace Log {
std::unordered_map<std::string_view, std::shared_ptr<spdlog::logger>> ALL_LOGGERS;
} // namespace Log
std::string GetCurrentThreadName() {
    return "test";
}
} // namespace Common

namespace {
std::mutex signaling_mutex;
bool peer_ready = false;
int resolve_calls = 0;
int failures = 0;
std::vector<s32> activations;
std::vector<s32> handshakes;
std::vector<s32> established;
std::function<void()> during_resolve;

void Check(bool condition, const char* expression, int line) {
    if (!condition) {
        std::printf("check failed at line %d: %s\n", line, expression);
        ++failures;
    }
}
#define CHECK(expression) Check((expression), #expression, __LINE__)
} // namespace

namespace Libraries::Net {
u16 PS4_SYSV_ABI sceNetNtohs(u16 value) {
    return static_cast<u16>((value >> 8) | (value << 8));
}
} // namespace Libraries::Net

namespace Libraries::Np::NpSignaling {
std::unordered_map<s32, ConnectionInfo> g_connections;
std::vector<PendingActivation> g_pending_activations;

SignalingMutexGuard::SignalingMutexGuard() {
    signaling_mutex.lock();
}
SignalingMutexGuard::~SignalingMutexGuard() {
    signaling_mutex.unlock();
}
void SendActivationRequestLocked(const ConnectionInfo& ci) {
    CHECK(ci.addr == 0x010012c6);
    CHECK(ci.port == 0x517a);
    activations.push_back(ci.conn_id);
}
void StartHandshakeInitiator(OrbisNpSignalingConnectionId conn_id) {
    handshakes.push_back(conn_id);
}
void EstablishConnection(OrbisNpSignalingConnectionId conn_id, bool peer_activated_hint) {
    established.push_back(conn_id);
}
namespace Stubs {
bool ResolvePeer(std::string_view online_id, u32* addr, u16* port) {
    ++resolve_calls;
    CHECK(online_id == "peer");
    if (during_resolve) {
        during_resolve();
    }
    if (!peer_ready) {
        return false;
    }
    *addr = 0x010012c6;
    *port = 0x517a;
    return true;
}
} // namespace Stubs
} // namespace Libraries::Np::NpSignaling

int main() {
    using namespace Libraries::Np::NpSignaling;
    auto reset = [] {
        g_connections.clear();
        g_pending_activations.clear();
        activations.clear();
        handshakes.clear();
        established.clear();
        during_resolve = {};
        peer_ready = false;
        resolve_calls = 0;
        auto& ci = g_connections[1];
        ci.conn_id = 1;
        ci.state = ConnState::SendingOffer;
        ci.locally_activated = true;
        QueueActivationLocked(1, "peer");
    };

    // Several dispatch ticks pass before ICE completes. No request is lost,
    // duplicated, or sent through a transport that is still connecting.
    reset();
    for (int tick = 0; tick < 3; ++tick) {
        ProcessPendingActivations();
        CHECK(g_pending_activations.size() == 1);
        CHECK(activations.empty());
        CHECK(handshakes.empty());
    }
    peer_ready = true;
    ProcessPendingActivations();
    ProcessPendingActivations();
    CHECK(resolve_calls == 4);
    CHECK(g_pending_activations.empty());
    CHECK(activations == std::vector<s32>{1});
    CHECK(handshakes == std::vector<s32>{1});

    // Timeout, local cancellation, and removal all stop a pending attempt.
    for (int cancellation = 0; cancellation < 3; ++cancellation) {
        reset();
        ProcessPendingActivations();
        if (cancellation == 0) {
            g_connections[1].state = ConnState::Inactive;
        } else if (cancellation == 1) {
            g_connections[1].locally_activated = false;
        } else {
            g_connections.erase(1);
        }
        peer_ready = true;
        ProcessPendingActivations();
        CHECK(resolve_calls == 1);
        CHECK(g_pending_activations.empty());
        CHECK(activations.empty());
        CHECK(handshakes.empty());
    }

    // Resolution runs outside the signaling lock. Cancellation can occur
    // before it returns, whether that resolution succeeds or remains pending.
    for (bool ready : {false, true}) {
        reset();
        peer_ready = ready;
        during_resolve = [] { g_connections[1].locally_activated = false; };
        ProcessPendingActivations();
        CHECK(g_pending_activations.empty());
        CHECK(activations.empty());
        CHECK(handshakes.empty());
    }

    // Reactivating an established connection does not restart its handshake.
    reset();
    g_pending_activations.clear();
    g_connections[1].state = ConnState::Established;
    QueueActivationLocked(1, "peer", false);
    ProcessPendingActivations();
    peer_ready = true;
    ProcessPendingActivations();
    CHECK(activations == std::vector<s32>{1});
    CHECK(handshakes.empty());
    CHECK(established == std::vector<s32>{1});

    std::printf("NP activation: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
