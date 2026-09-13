// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "common/types.h"
#include "shadnet/client.h"
#include "shadnet/peer_address.h"

// Deliberately no <juice/juice.h> here. Core includes this header, and the
// convention is that only src/shadnet sees libjuice. PeerConnection is held
// behind a unique_ptr to an incomplete type, which is why the destructor is
// defined out of line.

namespace ShadNet {

class PeerConnection;
class PendingPeerBegins;

// Owns every peer connection and the virtual-address table, and is the only
// place that knows a virtual address corresponds to an ICE agent.
//
// Above this, a peer is an address in 198.18.0.0/15 and nothing else. Below
// it, that address is a live ICE agent holding a NAT mapping. Keeping the two
// apart is what lets the emulated socket layer stay unaware of any of this.
class PeerTransport {
public:
    static PeerTransport& Instance();

    ~PeerTransport();

    PeerTransport(const PeerTransport&) = delete;
    PeerTransport& operator=(const PeerTransport&) = delete;

    // Receives one complete framed datagram -- vport header included, exactly
    // the bytes the sender passed to SendTo. from_addr_nbo is the sender's
    // virtual address, taken from the connection it arrived on and never from
    // anything inside the datagram.
    using FrameHandler = std::function<void(u32 from_addr_nbo, const u8* data, size_t size)>;

    void SetFrameHandler(FrameHandler handler);

    // Binds to an authenticated client: subscribes to the peer-session
    // notifications and fetches the ICE server list. Detach() tears every
    // connection down, which blocks until libjuice's threads have stopped.
    // title_id is part of the server's pairing key, so both peers must send
    // the same one or they open two sessions instead of joining one.
    void Attach(std::shared_ptr<ShadNetClient> client, std::string title_id);
    void Detach();

    // Replies to the peer commands (120-123), forwarded from the matching
    // reply dispatcher. The session a player begins arrives here, in the
    // reply; the other player learns of it from a notification instead. Both
    // paths converge on the same handler.
    void OnReply(CommandType cmd, u64 pkt_id, ErrorType error, const std::vector<u8>& body);

    // Sends one framed datagram to whoever owns dest_addr_nbo. Returns size,
    // or -1 when the address does not resolve or no path is up yet.
    //
    // There is deliberately no fallback to a real socket. An unresolved
    // virtual address is an internal inconsistency; sending it to the network
    // would put an RFC 2544 address on the wire, where it is either dropped
    // silently or delivered to a stranger.
    int SendTo(u32 dest_addr_nbo, const u8* data, size_t size);

    // Our own virtual address, or 0 before any session exists. Every session
    // leases the same one for the lifetime of the connection to the server.
    u32 LocalVirtualAddr() const;

    // The peer's virtual address, opening a session if there is not one yet.
    // Returns 0 while the session is still being set up: the caller is
    // expected to retry, which is what the activation path already does.
    //
    // A session that never connected, or failed, is replaced here rather than
    // returned; see IsPeerSessionStale.
    u32 ResolvePeer(std::string_view npid);

    // Whether a path to this address is up right now. A leased address is not
    // a connected one -- ICE may still be checking, or may have failed.
    bool IsPeerConnected(u32 addr_nbo) const;

    // For logging: "<local> | <remote>" of the nominated pair, empty if not
    // connected. The only evidence of which candidate type actually won.
    std::string SelectedPathFor(u32 addr_nbo) const;

private:
    PeerTransport();

    struct Session {
        std::unique_ptr<PeerConnection> connection;
        u32 generation = 0;
        std::string peer_npid;
        u32 peer_addr_nbo = 0;
        std::chrono::steady_clock::time_point opened_at{};
    };

    // A session to end on the server once the lock is released.
    struct EndedSession {
        u64 session_id = 0;
        u32 generation = 0;
    };

    void HandleSessionOpened(const NotifyPeerSessionOpened& notification);
    void HandlePeerSignal(const NotifyPeerSignal& notification);
    void HandleSessionClosed(const NotifyPeerSessionClosed& notification);

    // Takes a session and its address out of the tables and hands back its
    // connection, which the caller must destroy after releasing m_mutex.
    std::unique_ptr<PeerConnection> DetachSessionLocked(u64 session_id);

    // Removes a session and destroys its connection outside the lock:
    // ~PeerConnection joins libjuice's thread, which must not happen while
    // holding a lock that thread's callbacks take.
    void DropSession(u64 session_id);

    std::shared_ptr<ShadNetClient> ClientHandle() const;

    mutable std::mutex m_mutex;
    std::shared_ptr<ShadNetClient> m_client;
    FrameHandler m_on_frame;
    std::unordered_map<u64, Session> m_sessions;
    PeerAddressTable m_addresses;
    u32 m_local_virtual_addr = 0;
    std::string m_title_id;
    std::vector<IceServerEntry> m_ice_servers;
    u64 m_ice_servers_pkt_id = 0;

    // Begin requests in flight, so a caller that retries -- and the activation
    // path retries on a timer -- does not open a second session while the
    // first is still being set up. Behind a pointer only to keep libjuice out
    // of this header.
    std::unique_ptr<PendingPeerBegins> m_pending_begins;

    // Bumped when a session with this peer ends without connecting. It is
    // part of the server's pairing key, so a retry opens a genuinely new
    // session rather than rejoining the one that just failed.
    std::unordered_map<std::string, u32> m_attempts;
};

} // namespace ShadNet
