// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include "common/types.h"

namespace ShadNet {

// Maps between the IPv4-shaped identity the emulated game sees and the peer it
// names.
//
// The game's socket API is PS4-compatible and deals in sockaddr_in, but the
// real path to a peer may be IPv6 or a TURN relay, and it can change while the
// game is running. So the game is given a stable address out of 198.18.0.0/15
// instead, leased by the server for the life of the session. These addresses
// are identity, not routing: nothing ever hands one to a real socket, and no
// OS interface, route or adapter is created for the range.
class PeerAddressTable {
public:
    // addr_nbo is network byte order throughout this class, matching what the
    // game passes to sendto and reads back from recvfrom. The server sends
    // these values in host byte order; converting once, at the call site that
    // receives the notification, is deliberate.
    void Insert(u32 addr_nbo, const std::string& npid, u64 session_id);
    void RemoveSession(u64 session_id);
    void Clear();

    std::optional<std::string> NpidFor(u32 addr_nbo) const;
    std::optional<u64> SessionFor(u32 addr_nbo) const;
    std::optional<u32> AddrFor(const std::string& npid) const;

    // True when addr_nbo falls inside 198.18.0.0/15. A false answer means the
    // address came from somewhere other than a peer session, which is a bug at
    // the caller rather than something to route.
    static bool IsVirtual(u32 addr_nbo);

private:
    struct Entry {
        std::string npid;
        u64 session_id = 0;
    };

    mutable std::mutex m_mutex;
    std::unordered_map<u32, Entry> m_by_addr;
    std::unordered_map<std::string, u32> m_by_npid;
};

} // namespace ShadNet
