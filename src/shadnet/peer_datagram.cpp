// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "shadnet/peer_datagram.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace ShadNet {

bool SplitPeerDatagram(const u8* data, size_t size, u16 message_id, const PeerWireSender& send) {
    if (data == nullptr || size == 0 || size > kPeerMaxDatagram) {
        return false;
    }

    const size_t count = (size + kPeerMaxPieceSize - 1) / kPeerMaxPieceSize;
    std::array<u8, kPeerMaxWireDatagram> wire{};
    wire[0] = kPeerDatagramMagic;
    wire[1] = static_cast<u8>(message_id >> 8);
    wire[2] = static_cast<u8>(message_id & 0xFF);
    wire[4] = static_cast<u8>(count);

    for (size_t index = 0; index < count; ++index) {
        const size_t offset = index * kPeerMaxPieceSize;
        const size_t piece_size = std::min(kPeerMaxPieceSize, size - offset);
        wire[3] = static_cast<u8>(index);
        std::memcpy(wire.data() + kPeerDatagramHeaderSize, data + offset, piece_size);
        if (!send(wire.data(), kPeerDatagramHeaderSize + piece_size)) {
            return false;
        }
    }
    return true;
}

bool PeerDatagramReassembler::Accept(const u8* data, size_t size, Clock::time_point now,
                                     const Deliver& deliver) {
    if (data == nullptr || size <= kPeerDatagramHeaderSize || data[0] != kPeerDatagramMagic) {
        return false;
    }
    const u16 message_id = static_cast<u16>((data[1] << 8) | data[2]);
    const u8 index = data[3];
    const u8 count = data[4];
    const u8* piece = data + kPeerDatagramHeaderSize;
    const size_t piece_size = size - kPeerDatagramHeaderSize;
    if (count == 0 || count > kPeerMaxPieces || index >= count || piece_size > kPeerMaxPieceSize) {
        return false;
    }

    if (count == 1) {
        // The common case, and nothing to hold: deliver straight from the input.
        deliver(piece, piece_size);
        return true;
    }

    Expire(now);

    auto it = std::find_if(m_pending.begin(), m_pending.end(),
                           [message_id](const Pending& p) { return p.message_id == message_id; });
    if (it != m_pending.end() && it->count != count) {
        // The sender has moved on to a new datagram under the same id, so the
        // old one can no longer complete.
        m_pending.erase(it);
        ++m_abandoned;
        it = m_pending.end();
    }
    if (it == m_pending.end()) {
        if (m_pending.size() >= kMaxPending) {
            m_pending.erase(m_pending.begin());
            ++m_abandoned;
        }
        Pending pending;
        pending.message_id = message_id;
        pending.count = count;
        pending.first_seen = now;
        pending.pieces.resize(count);
        m_pending.push_back(std::move(pending));
        it = std::prev(m_pending.end());
    }

    std::vector<u8>& slot = it->pieces[index];
    if (!slot.empty()) {
        return true;
    }
    slot.assign(piece, piece + piece_size);
    if (++it->received < it->count) {
        return true;
    }

    size_t total = 0;
    for (const std::vector<u8>& p : it->pieces) {
        total += p.size();
    }
    std::vector<u8> whole;
    whole.reserve(total);
    for (const std::vector<u8>& p : it->pieces) {
        whole.insert(whole.end(), p.begin(), p.end());
    }
    m_pending.erase(it);

    if (total > kPeerMaxDatagram) {
        ++m_abandoned;
        return false;
    }
    deliver(whole.data(), whole.size());
    return true;
}

void PeerDatagramReassembler::Expire(Clock::time_point now) {
    const auto stale = std::remove_if(m_pending.begin(), m_pending.end(), [now](const Pending& p) {
        return now - p.first_seen >= kTimeout;
    });
    m_abandoned += static_cast<u64>(std::distance(stale, m_pending.end()));
    m_pending.erase(stale, m_pending.end());
}

size_t PeerDatagramReassembler::PendingCount() const {
    return m_pending.size();
}

u64 PeerDatagramReassembler::AbandonedCount() const {
    return m_abandoned;
}

} // namespace ShadNet
