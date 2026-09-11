// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Proves the vendored ICE dependency works before any protocol depends on it.
// Two agents negotiate in-process -- the "signaling channel" is a direct call
// into the peer -- gather against a public STUN server, and exchange a payload.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#include <juice/juice.h>

namespace {

struct Endpoint {
    const char* name = "";
    juice_agent_t* agent = nullptr;
    Endpoint* peer = nullptr;
    std::atomic<bool> connected{false};
    std::atomic<bool> failed{false};
    std::atomic<bool> got_payload{false};
};

void OnStateChanged(juice_agent_t*, juice_state_t state, void* user) {
    auto* self = static_cast<Endpoint*>(user);
    std::printf("[%s] state -> %s\n", self->name, juice_state_to_string(state));
    if (state == JUICE_STATE_CONNECTED || state == JUICE_STATE_COMPLETED) {
        self->connected.store(true);
    } else if (state == JUICE_STATE_FAILED) {
        self->failed.store(true);
    }
}

void OnCandidate(juice_agent_t*, const char* sdp, void* user) {
    auto* self = static_cast<Endpoint*>(user);
    std::printf("[%s] local candidate: %s\n", self->name, sdp);
    juice_add_remote_candidate(self->peer->agent, sdp);
}

void OnGatheringDone(juice_agent_t*, void* user) {
    auto* self = static_cast<Endpoint*>(user);
    std::printf("[%s] gathering done\n", self->name);
    juice_set_remote_gathering_done(self->peer->agent);
}

void OnRecv(juice_agent_t*, const char* data, size_t size, void* user) {
    auto* self = static_cast<Endpoint*>(user);
    std::printf("[%s] received %zu bytes\n", self->name, size);
    if (size == 5 && std::memcmp(data, "hello", 5) == 0) {
        self->got_payload.store(true);
    }
}

juice_agent_t* MakeAgent(Endpoint* endpoint) {
    juice_config_t config{};
    config.stun_server_host = "stun.l.google.com";
    config.stun_server_port = 19302;
    config.cb_state_changed = OnStateChanged;
    config.cb_candidate = OnCandidate;
    config.cb_gathering_done = OnGatheringDone;
    config.cb_recv = OnRecv;
    config.user_ptr = endpoint;
    return juice_create(&config);
}

bool WaitFor(const std::atomic<bool>& a, const std::atomic<bool>& b, int timeout_seconds) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        if (a.load() && b.load()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

} // namespace

int main() {
    juice_set_log_level(JUICE_LOG_LEVEL_WARN);

    Endpoint a, b;
    a.name = "A";
    b.name = "B";
    a.peer = &b;
    b.peer = &a;
    a.agent = MakeAgent(&a);
    b.agent = MakeAgent(&b);
    if (a.agent == nullptr || b.agent == nullptr) {
        std::printf("FAIL: juice_create returned null\n");
        return 1;
    }

    char sdp_a[JUICE_MAX_SDP_STRING_LEN]{};
    char sdp_b[JUICE_MAX_SDP_STRING_LEN]{};
    juice_get_local_description(a.agent, sdp_a, sizeof(sdp_a));
    juice_get_local_description(b.agent, sdp_b, sizeof(sdp_b));
    juice_set_remote_description(a.agent, sdp_b);
    juice_set_remote_description(b.agent, sdp_a);

    juice_gather_candidates(a.agent);
    juice_gather_candidates(b.agent);

    const auto started = std::chrono::steady_clock::now();
    if (!WaitFor(a.connected, b.connected, 20)) {
        std::printf("FAIL: agents did not reach CONNECTED (a=%d b=%d, failed a=%d b=%d)\n",
                    a.connected.load(), b.connected.load(), a.failed.load(), b.failed.load());
        juice_destroy(a.agent);
        juice_destroy(b.agent);
        return 1;
    }
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - started)
                                .count();

    char local[JUICE_MAX_CANDIDATE_SDP_STRING_LEN]{};
    char remote[JUICE_MAX_CANDIDATE_SDP_STRING_LEN]{};
    juice_get_selected_candidates(a.agent, local, sizeof(local), remote, sizeof(remote));
    std::printf("selected after %lldms:\n  local  %s\n  remote %s\n",
                static_cast<long long>(elapsed_ms), local, remote);

    juice_send(a.agent, "hello", 5);
    const auto payload_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < payload_deadline && !b.got_payload.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    const bool delivered = b.got_payload.load();
    juice_destroy(a.agent);
    juice_destroy(b.agent);

    if (!delivered) {
        std::printf("FAIL: payload not delivered\n");
        return 1;
    }
    std::printf("PASS\n");
    return 0;
}
