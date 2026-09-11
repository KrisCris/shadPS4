// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Writes canonical peer-connectivity messages, encoded with the CLIENT's copy
// of shadnet.proto, to the file named on the command line.
//
// The server decodes the same file with its own copy and asserts every field
// (tests/test_peer_proto_decode.cpp in the shadNet repo). The two .proto files
// are maintained by hand, so a field-number drift between them would corrupt
// the wire silently rather than failing to compile. A round trip through one
// side's own generated code would not catch that; only crossing the boundary
// does.
//
// Each record is [u32 LE length][bytes], in the order the decoder expects.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "shadnet.pb.h"

namespace {

void Append(std::vector<uint8_t>& out, const std::string& bytes) {
    const uint32_t length = static_cast<uint32_t>(bytes.size());
    out.push_back(static_cast<uint8_t>(length & 0xFF));
    out.push_back(static_cast<uint8_t>((length >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((length >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((length >> 24) & 0xFF));
    out.insert(out.end(), bytes.begin(), bytes.end());
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: peer_proto_encode <output-file>\n");
        return 2;
    }

    std::vector<uint8_t> out;

    {
        shadnet::PeerSessionBeginRequest msg;
        msg.set_target_npid("MintCoffeeCat");
        msg.set_title_id("CUSA03173");
        msg.set_attempt(7);
        Append(out, msg.SerializeAsString());
    }
    {
        shadnet::PeerSessionBeginReply msg;
        msg.set_session_id(0x0123456789ABCDEFull);
        msg.set_generation(3);
        msg.set_is_offerer(true);
        // 198.18.0.5 and 198.18.1.42, host byte order, as the server sends them.
        msg.set_local_virtual_addr(0xC6120005u);
        msg.set_peer_virtual_addr(0xC612012Au);
        msg.set_peer_npid("connlost");
        Append(out, msg.SerializeAsString());
    }
    {
        shadnet::PeerSignalRequest msg;
        msg.set_session_id(0x0123456789ABCDEFull);
        msg.set_generation(3);
        msg.set_kind(shadnet::PEER_SIGNAL_CANDIDATE);
        msg.set_payload("a=candidate:1 1 UDP 2114977791 10.0.0.246 50497 typ host");
        Append(out, msg.SerializeAsString());
    }
    {
        shadnet::NotifyPeerSessionOpened msg;
        msg.set_session_id(0x0123456789ABCDEFull);
        msg.set_generation(3);
        msg.set_is_offerer(false);
        msg.set_peer_npid("MintCoffeeCat");
        msg.set_local_virtual_addr(0xC612012Au);
        msg.set_peer_virtual_addr(0xC6120005u);
        msg.set_title_id("CUSA03173");
        Append(out, msg.SerializeAsString());
    }
    {
        shadnet::NotifyPeerSignal msg;
        msg.set_session_id(0x0123456789ABCDEFull);
        msg.set_generation(3);
        msg.set_kind(shadnet::PEER_SIGNAL_GATHERING_DONE);
        msg.set_payload("");
        msg.set_from_npid("connlost");
        Append(out, msg.SerializeAsString());
    }
    {
        shadnet::NotifyPeerSessionClosed msg;
        msg.set_session_id(0x0123456789ABCDEFull);
        msg.set_generation(3);
        msg.set_reason(2);
        Append(out, msg.SerializeAsString());
    }
    {
        shadnet::GetIceServersReply msg;
        shadnet::IceServer* stun = msg.add_servers();
        stun->set_host("stun.example.org");
        stun->set_port(3478);
        stun->set_is_turn(false);
        shadnet::IceServer* turn = msg.add_servers();
        turn->set_host("turn.example.org");
        turn->set_port(3479);
        turn->set_is_turn(true);
        turn->set_username("1757552400:connlost");
        turn->set_credential("QUGSNM71FCId8R1W5qOaN9uMm0M=");
        turn->set_expires_at(1757552400ull);
        Append(out, msg.SerializeAsString());
    }

    std::FILE* file = std::fopen(argv[1], "wb");
    if (file == nullptr) {
        std::fprintf(stderr, "cannot open %s for writing\n", argv[1]);
        return 1;
    }
    const size_t written = std::fwrite(out.data(), 1, out.size(), file);
    std::fclose(file);
    if (written != out.size()) {
        std::fprintf(stderr, "short write to %s\n", argv[1]);
        return 1;
    }

    std::printf("wrote %zu bytes to %s\n", out.size(), argv[1]);
    return 0;
}
