// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Splitting and reassembly of game datagrams, without a network. The sizes
// are the ones that matter in the field: 2800 is the Chalice Dungeon join that
// failed with JUICE_ERR_TOO_LARGE, 9184 is what the game sizes its P2P receive
// buffer to, and 65507 is the largest a P2P socket accepts.

#include <algorithm>
#include <cstdio>
#include <vector>

#include "shadnet/peer_datagram.h"

namespace {

using ShadNet::PeerDatagramReassembler;
using Clock = PeerDatagramReassembler::Clock;
using Wire = std::vector<std::vector<u8>>;

int g_failures = 0;

bool Check(bool condition, const char* expression, int line) {
    if (!condition) {
        std::printf("check failed at line %d: %s\n", line, expression);
        ++g_failures;
    }
    return condition;
}

#define CHECK(expression) Check((expression), #expression, __LINE__)

std::vector<u8> Pattern(size_t size) {
    std::vector<u8> data(size);
    for (size_t i = 0; i < size; ++i) {
        data[i] = static_cast<u8>(i * 31 + size);
    }
    return data;
}

Wire Split(const std::vector<u8>& data, u16 message_id) {
    Wire wire;
    const bool ok = ShadNet::SplitPeerDatagram(data.data(), data.size(), message_id,
                                               [&wire](const u8* piece, size_t size) {
                                                   wire.emplace_back(piece, piece + size);
                                                   return true;
                                               });
    CHECK(ok);
    return wire;
}

// Feeds wire datagrams in the given order and collects whatever completes.
std::vector<std::vector<u8>> Feed(PeerDatagramReassembler& reassembler, const Wire& wire,
                                  Clock::time_point now = Clock::now()) {
    std::vector<std::vector<u8>> delivered;
    for (const std::vector<u8>& piece : wire) {
        CHECK(reassembler.Accept(piece.data(), piece.size(), now,
                                 [&delivered](const u8* data, size_t size) {
                                     delivered.emplace_back(data, data + size);
                                 }));
    }
    return delivered;
}

void RoundTrip(size_t size, size_t expected_pieces) {
    const std::vector<u8> data = Pattern(size);
    const Wire wire = Split(data, 7);
    if (!CHECK(wire.size() == expected_pieces)) {
        std::printf("  size %zu split into %zu pieces, want %zu\n", size, wire.size(),
                    expected_pieces);
    }
    for (const std::vector<u8>& piece : wire) {
        CHECK(piece.size() <= ShadNet::kPeerMaxWireDatagram);
        CHECK(piece[0] == ShadNet::kPeerDatagramMagic);
        CHECK(piece[4] == wire.size());
    }

    PeerDatagramReassembler reassembler;
    const auto delivered = Feed(reassembler, wire);
    if (CHECK(delivered.size() == 1)) {
        CHECK(delivered[0] == data);
    }
    CHECK(reassembler.PendingCount() == 0);
}

} // namespace

int main() {
    using ShadNet::kPeerMaxDatagram;
    using ShadNet::kPeerMaxPieces;
    using ShadNet::kPeerMaxPieceSize;

    // Every size the game can send has to fit in a count byte.
    CHECK(kPeerMaxPieces <= 255);

    RoundTrip(1, 1);
    RoundTrip(kPeerMaxPieceSize, 1);
    RoundTrip(kPeerMaxPieceSize + 1, 2);
    RoundTrip(2800, 3);
    RoundTrip(9184, 9);
    RoundTrip(kPeerMaxDatagram, kPeerMaxPieces);

    // Sizes a P2P socket could never hand over are refused, not truncated.
    {
        int calls = 0;
        const auto count_calls = [&calls](const u8*, size_t) {
            ++calls;
            return true;
        };
        const std::vector<u8> too_big = Pattern(kPeerMaxDatagram + 1);
        CHECK(!ShadNet::SplitPeerDatagram(too_big.data(), too_big.size(), 1, count_calls));
        CHECK(!ShadNet::SplitPeerDatagram(too_big.data(), 0, 1, count_calls));
        CHECK(!ShadNet::SplitPeerDatagram(nullptr, 10, 1, count_calls));
        CHECK(calls == 0);
    }

    // A failed send stops the split: later pieces cannot rescue the datagram.
    {
        int calls = 0;
        const std::vector<u8> data = Pattern(9184);
        const bool ok = ShadNet::SplitPeerDatagram(
            data.data(), data.size(), 1, [&calls](const u8*, size_t) { return ++calls < 2; });
        CHECK(!ok);
        CHECK(calls == 2);
    }

    // Any arrival order reassembles, since UDP promises none.
    {
        const std::vector<u8> data = Pattern(9184);
        Wire wire = Split(data, 300);
        std::reverse(wire.begin(), wire.end());
        std::swap(wire[2], wire[5]);
        PeerDatagramReassembler reassembler;
        const auto delivered = Feed(reassembler, wire);
        if (CHECK(delivered.size() == 1)) {
            CHECK(delivered[0] == data);
        }
    }

    // A duplicated piece neither delivers twice nor corrupts the result.
    {
        const std::vector<u8> data = Pattern(2800);
        Wire wire = Split(data, 9);
        wire.insert(wire.begin() + 1, wire[0]);
        wire.push_back(wire.back());
        PeerDatagramReassembler reassembler;
        const auto delivered = Feed(reassembler, wire);
        if (CHECK(delivered.size() == 1)) {
            CHECK(delivered[0] == data);
        }
    }

    // Two datagrams interleaved, as concurrent sends from two game threads are.
    {
        const std::vector<u8> first = Pattern(2800);
        const std::vector<u8> second = Pattern(4000);
        const Wire a = Split(first, 1);
        const Wire b = Split(second, 2);
        Wire mixed;
        for (size_t i = 0; i < std::max(a.size(), b.size()); ++i) {
            if (i < b.size()) {
                mixed.push_back(b[i]);
            }
            if (i < a.size()) {
                mixed.push_back(a[i]);
            }
        }
        PeerDatagramReassembler reassembler;
        const auto delivered = Feed(reassembler, mixed);
        if (CHECK(delivered.size() == 2)) {
            CHECK(delivered[0] == first);
            CHECK(delivered[1] == second);
        }
        CHECK(reassembler.PendingCount() == 0);
    }

    // A lost piece loses the datagram, and what was held is let go in time.
    {
        const auto start = Clock::now();
        const std::vector<u8> data = Pattern(2800);
        Wire wire = Split(data, 44);
        const std::vector<u8> late = wire.back();
        wire.pop_back();
        PeerDatagramReassembler reassembler;
        CHECK(Feed(reassembler, wire, start).empty());
        CHECK(reassembler.PendingCount() == 1);

        const auto after = start + PeerDatagramReassembler::kTimeout;
        CHECK(Feed(reassembler, Split(Pattern(2000), 45), after).size() == 1);
        CHECK(reassembler.AbandonedCount() == 1);
        CHECK(reassembler.PendingCount() == 0);

        // The straggler cannot complete the abandoned datagram on its own.
        CHECK(Feed(reassembler, {late}, after).empty());
    }

    // Many incomplete datagrams cannot grow memory without bound.
    {
        PeerDatagramReassembler reassembler;
        const auto now = Clock::now();
        for (u16 id = 0; id < PeerDatagramReassembler::kMaxPending + 4; ++id) {
            Wire wire = Split(Pattern(2800), id);
            wire.pop_back();
            CHECK(Feed(reassembler, wire, now).empty());
        }
        CHECK(reassembler.PendingCount() == PeerDatagramReassembler::kMaxPending);
        CHECK(reassembler.AbandonedCount() == 4);
    }

    // An id reused for a datagram of a different shape replaces the stale one.
    {
        PeerDatagramReassembler reassembler;
        Wire stale = Split(Pattern(9184), 5);
        stale.pop_back();
        CHECK(Feed(reassembler, stale).empty());
        const std::vector<u8> fresh = Pattern(2800);
        const auto delivered = Feed(reassembler, Split(fresh, 5));
        if (CHECK(delivered.size() == 1)) {
            CHECK(delivered[0] == fresh);
        }
        CHECK(reassembler.AbandonedCount() == 1);
    }

    // Malformed input is refused and delivers nothing. The first case is what
    // a peer on an older build sends: a bare vport-framed datagram.
    {
        const std::vector<u8> good = Split(Pattern(2800), 3)[0];
        std::vector<std::vector<u8>> bad;
        bad.push_back({0x0E, 0x4A, 0x0E, 0x4A, 1, 2, 3});
        bad.push_back(std::vector<u8>(good.begin(), good.begin() + 5));
        auto count_zero = good;
        count_zero[4] = 0;
        bad.push_back(count_zero);
        auto index_past = good;
        index_past[3] = index_past[4];
        bad.push_back(index_past);
        auto too_many = good;
        too_many[4] = static_cast<u8>(kPeerMaxPieces + 1);
        bad.push_back(too_many);
        auto oversized = good;
        oversized.push_back(0);
        bad.push_back(oversized);

        PeerDatagramReassembler reassembler;
        int delivered = 0;
        for (const std::vector<u8>& piece : bad) {
            CHECK(!reassembler.Accept(piece.data(), piece.size(), Clock::now(),
                                      [&delivered](const u8*, size_t) { ++delivered; }));
        }
        CHECK(!reassembler.Accept(nullptr, 0, Clock::now(), [](const u8*, size_t) {}));
        CHECK(delivered == 0);
        CHECK(reassembler.PendingCount() == 0);
    }

    if (g_failures != 0) {
        std::printf("FAIL: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("PASS\n");
    return 0;
}
