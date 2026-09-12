// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/logging/log.h"
#include "core/libraries/network/net.h"
#include "core/libraries/np/np_signaling/np_signaling_state.h"
#include "core/libraries/np/np_signaling/np_signaling_stubs.h"

namespace Libraries::Np::NpSignaling {

using Libraries::Net::sceNetNtohs;

void QueueActivationLocked(OrbisNpSignalingConnectionId conn_id, std::string_view peer_online_id,
                           bool start_handshake) {
    g_pending_activations.push_back({conn_id, std::string(peer_online_id), start_handshake});
}

void ProcessPendingActivations() {
    std::vector<PendingActivation> work;
    {
        SignalingMutexGuard lock;
        if (g_pending_activations.empty()) {
            return;
        }
        work.swap(g_pending_activations);
    }

    for (const PendingActivation& act : work) {
        bool already_established = false;
        {
            SignalingMutexGuard lock;
            const auto it = g_connections.find(act.conn_id);
            if (it == g_connections.end() || it->second.state == ConnState::Inactive ||
                !it->second.locally_activated) {
                continue;
            }
        }

        u32 peer_addr = 0;
        u16 peer_port = 0;
        const bool resolved = Stubs::ResolvePeer(act.peer_online_id, &peer_addr, &peer_port) &&
                              peer_addr != 0 && peer_port != 0;

        if (!resolved) {
            // Peer discovery is asynchronous. Keep the activation pending until the next
            // dispatch tick; the existing connection timeout still bounds the whole attempt.
            SignalingMutexGuard lock;
            const auto it = g_connections.find(act.conn_id);
            if (it != g_connections.end() && it->second.state != ConnState::Inactive &&
                it->second.locally_activated) {
                g_pending_activations.push_back(act);
            }
            continue;
        }

        {
            SignalingMutexGuard lock;
            const auto it = g_connections.find(act.conn_id);
            if (it == g_connections.end() || it->second.state == ConnState::Inactive ||
                !it->second.locally_activated) {
                continue;
            }
            it->second.addr = peer_addr;
            it->second.port = peer_port;
            SendActivationRequestLocked(it->second);
            already_established = it->second.state == ConnState::Established;
            LOG_INFO(Lib_NpSignaling, "connection {} sent local activation to '{}' at {:#x}:{}{}",
                     act.conn_id, act.peer_online_id, peer_addr, sceNetNtohs(peer_port),
                     act.start_handshake ? "" : " (existing connection)");
        }
        if (act.start_handshake) {
            StartHandshakeInitiator(act.conn_id);
        } else if (already_established) {
            EstablishConnection(act.conn_id, false);
        }
    }
}

} // namespace Libraries::Np::NpSignaling
