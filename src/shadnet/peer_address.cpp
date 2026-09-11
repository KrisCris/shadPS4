// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "shadnet/peer_address.h"

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

namespace ShadNet {

namespace {
constexpr u32 kVirtualRangeFirst = 0xC6120000u; // 198.18.0.0
constexpr u32 kVirtualRangeLast = 0xC613FFFFu;  // 198.19.255.255
} // namespace

void PeerAddressTable::Insert(u32 addr_nbo, const std::string& npid, u64 session_id) {
    std::lock_guard lock(m_mutex);

    // Re-ringing the bell leases the same peer a new address. Both directions
    // have to be rewritten together: dropping only the forward entry leaves
    // the old address resolving to a peer that no longer answers there.
    const auto previous = m_by_npid.find(npid);
    if (previous != m_by_npid.end() && previous->second != addr_nbo) {
        m_by_addr.erase(previous->second);
    }

    m_by_addr[addr_nbo] = Entry{npid, session_id};
    m_by_npid[npid] = addr_nbo;
}

void PeerAddressTable::RemoveSession(u64 session_id) {
    std::lock_guard lock(m_mutex);
    for (auto it = m_by_addr.begin(); it != m_by_addr.end();) {
        if (it->second.session_id != session_id) {
            ++it;
            continue;
        }
        // Only drop the reverse entry if it still points at this address; a
        // newer session may already have re-leased the same peer.
        const auto reverse = m_by_npid.find(it->second.npid);
        if (reverse != m_by_npid.end() && reverse->second == it->first) {
            m_by_npid.erase(reverse);
        }
        it = m_by_addr.erase(it);
    }
}

void PeerAddressTable::Clear() {
    std::lock_guard lock(m_mutex);
    m_by_addr.clear();
    m_by_npid.clear();
}

std::optional<std::string> PeerAddressTable::NpidFor(u32 addr_nbo) const {
    std::lock_guard lock(m_mutex);
    const auto it = m_by_addr.find(addr_nbo);
    if (it == m_by_addr.end()) {
        return std::nullopt;
    }
    return it->second.npid;
}

std::optional<u64> PeerAddressTable::SessionFor(u32 addr_nbo) const {
    std::lock_guard lock(m_mutex);
    const auto it = m_by_addr.find(addr_nbo);
    if (it == m_by_addr.end()) {
        return std::nullopt;
    }
    return it->second.session_id;
}

std::optional<u32> PeerAddressTable::AddrFor(const std::string& npid) const {
    std::lock_guard lock(m_mutex);
    const auto it = m_by_npid.find(npid);
    if (it == m_by_npid.end()) {
        return std::nullopt;
    }
    return it->second;
}

bool PeerAddressTable::IsVirtual(u32 addr_nbo) {
    const u32 host_order = ntohl(addr_nbo);
    return host_order >= kVirtualRangeFirst && host_order <= kVirtualRangeLast;
}

} // namespace ShadNet
