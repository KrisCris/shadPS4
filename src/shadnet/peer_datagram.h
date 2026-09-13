// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <vector>

#include "common/types.h"

// Game datagrams are split into pieces that fit any Internet path whole, and
// put back together on the far side before the game sees them.
//
// libjuice sets DF on its sockets, so a datagram larger than the path MTU is
// refused by the sender's own stack (EMSGSIZE, JUICE_ERR_TOO_LARGE) instead of
// being fragmented by IP. The game routinely sends more than that: joining a
// Chalice Dungeon sends 2800 bytes, and its P2P sockets are sized for 9184.
// libjuice's receive buffer is also only 4096 bytes, and a datagram past it
// fails the receive outright. Splitting here fixes both, and avoids IP
// fragments, which NATs and firewalls routinely drop.
//
// Every wire datagram carries this header, then its piece of the datagram:
//   u8  magic       kPeerDatagramMagic
//   u16 message_id  big-endian, chosen by the sender
//   u8  index       0-based
//   u8  count       1 when the datagram fits in one piece
//
// Both peers must speak it. The magic is there so a peer on an older build
// shows up as a clear warning rather than as garbled game traffic.

namespace ShadNet {

inline constexpr u8 kPeerDatagramMagic = 0xB1;
inline constexpr size_t kPeerDatagramHeaderSize = 5;

// 1280 is the IPv6 minimum MTU, and what tunnels such as Cloudflare WARP use.
//
// Budget for the worst relay encapsulation, not the cheapest. A relayed piece
// reaches an allocation's owner as a TURN Data indication, not ChannelData,
// whenever the owner never bound a channel to that peer -- the normal case
// when only the other side sends through the relay. That adds a STUN header
// (20), XOR-PEER-ADDRESS (24 for IPv6), the DATA header and padding (7),
// SOFTWARE (24 for coturn 4.6) and FINGERPRINT (8): 83 bytes. With 48 for IPv6
// and UDP, 1149 remain; 1100 leaves room for a longer SOFTWARE string.
inline constexpr size_t kPeerMaxWireDatagram = 1100;
inline constexpr size_t kPeerMaxPieceSize = kPeerMaxWireDatagram - kPeerDatagramHeaderSize;

// The largest UDP payload over IPv4, and so the largest datagram a game can
// have handed to a P2P socket on real hardware.
inline constexpr size_t kPeerMaxDatagram = 65507;
inline constexpr size_t kPeerMaxPieces =
    (kPeerMaxDatagram + kPeerMaxPieceSize - 1) / kPeerMaxPieceSize;

// Returns false to stop: the datagram is lost, as it would be on a real network.
using PeerWireSender = std::function<bool(const u8* data, size_t size)>;

// Hands each wire datagram to send, in order. Returns false if size is 0 or
// above kPeerMaxDatagram, or as soon as send returns false.
bool SplitPeerDatagram(const u8* data, size_t size, u16 message_id, const PeerWireSender& send);

// Reassembles what SplitPeerDatagram produced. Not thread-safe: feed it from
// one thread, which libjuice's callbacks for one agent already are.
class PeerDatagramReassembler {
public:
    using Clock = std::chrono::steady_clock;
    using Deliver = std::function<void(const u8* data, size_t size)>;

    // A datagram still missing pieces after this is abandoned. Losing any
    // piece loses the datagram, as losing an IP fragment would.
    static constexpr Clock::duration kTimeout = std::chrono::seconds(2);
    // Incomplete datagrams held at once; past this the oldest is abandoned.
    static constexpr size_t kMaxPending = 16;

    // Feeds one wire datagram. Calls deliver synchronously, at most once, with
    // a complete datagram. Returns false if the input was not a valid piece.
    bool Accept(const u8* data, size_t size, Clock::time_point now, const Deliver& deliver);

    size_t PendingCount() const;
    u64 AbandonedCount() const;

private:
    struct Pending {
        u16 message_id = 0;
        u8 count = 0;
        u8 received = 0;
        Clock::time_point first_seen{};
        // Empty until that piece arrives; a valid piece is never empty.
        std::vector<std::vector<u8>> pieces;
    };

    void Expire(Clock::time_point now);

    // In arrival order, so the front is always the oldest.
    std::vector<Pending> m_pending;
    u64 m_abandoned = 0;
};

} // namespace ShadNet
