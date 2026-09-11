// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// One peer of a two-peer ICE exchange, as a standalone process, so the pair
// can be placed in separate network namespaces with real NATs between them.
//
// peer_connection_test drives both agents inside one process on one host. That
// proves the state machine and the trickle ordering, and it proved nothing
// about NAT traversal: both agents saw the same interfaces and settled on a
// host pair. Everything this file exists to measure -- whether a server
// reflexive candidate is discovered, whether a relayed one is used when the
// direct path is blocked, whether an IPv6-only path stays IPv6 -- needs the
// two peers to be somewhere genuinely different.
//
// Three modes, one binary, so a namespace only needs one file:
//
//   --role broker     rendezvous; forwards signaling blobs between two agents
//   --role offerer    describes itself first, becomes ICE controlling
//   --role answerer   waits for the offer, becomes ICE controlled
//
// The broker stands in for shadNet's PeerSignal relay. It is deliberately not
// the real server: what is under test here is which candidate wins, and the
// server's pairing, generation and authorisation rules are covered
// deterministically by the shadNet repo's own test_peer_sessions. Using a
// stand-in keeps a failure in this matrix attributable to ICE.
//
// The exit code is the assertion. An agent exits 0 only when it connected,
// carried a datagram both ways, saw no role conflict, and the nominated pair
// matched --expect-local / --expect-remote.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#define SHUT_RDWR SD_BOTH
using socket_t = SOCKET;
#define CLOSE_SOCKET closesocket
#define INVALID_SOCK INVALID_SOCKET
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
#define CLOSE_SOCKET ::close
#define INVALID_SOCK (-1)
#endif

#include "common/logging/log.h"
#include "shadnet/client.h"
#include "shadnet/peer_connection.h"

// The emulator's logging implementation is not linked here, for the same
// reason peer_connection_test does not link it: it reaches into Core headers
// that use clang-only attributes. The LOG_ macros look up a logger and do
// nothing when it is null, which is always the case here.
namespace Common {
namespace Log {
std::unordered_map<std::string_view, std::shared_ptr<spdlog::logger>> ALL_LOGGERS;
} // namespace Log
std::string GetCurrentThreadName() {
    return "netns-agent";
}
} // namespace Common

namespace {

using ShadNet::IceServerEntry;
using ShadNet::PeerConnection;
using ShadNet::PeerSignalKind;
using ShadNet::PeerTransportState;

// Two virtual addresses from the same range the transport leases, so the
// datagram assertions below check the identity the real code would carry.
constexpr u32 kOffererVirtualAddr = 0xC6120005u;  // 198.18.0.5
constexpr u32 kAnswererVirtualAddr = 0xC612012Au; // 198.18.1.42

std::atomic<bool> g_role_conflict{false};
juice_log_level_t g_juice_print_level = JUICE_LOG_LEVEL_WARN;

void OnJuiceLog(juice_log_level_t level, const char* message) {
    if (message == nullptr) {
        return;
    }
    if (std::strstr(message, "role conflict") != nullptr) {
        g_role_conflict.store(true);
        std::printf("juice: %s\n", message);
        return;
    }
    if (level >= g_juice_print_level) {
        std::printf("juice: %s\n", message);
    }
}

juice_log_level_t JuiceLogLevel(const std::string& name) {
    if (name == "none") {
        return JUICE_LOG_LEVEL_NONE;
    }
    if (name == "info") {
        return JUICE_LOG_LEVEL_INFO;
    }
    if (name == "debug") {
        return JUICE_LOG_LEVEL_DEBUG;
    }
    if (name == "verbose") {
        return JUICE_LOG_LEVEL_VERBOSE;
    }
    return JUICE_LOG_LEVEL_WARN;
}

bool SocketStartup() {
#ifdef _WIN32
    WSADATA data;
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
#else
    return true;
#endif
}

// ---------------------------------------------------------------------------
// Signaling framing: [u8 kind][u32 length, network order][payload]
// ---------------------------------------------------------------------------

constexpr size_t kFrameHeaderSize = 5;

bool SendAll(socket_t sock, const u8* data, size_t size) {
    size_t sent = 0;
    while (sent < size) {
        const int rc = static_cast<int>(::send(sock, reinterpret_cast<const char*>(data + sent),
                                               static_cast<int>(size - sent), 0));
        if (rc <= 0) {
            return false;
        }
        sent += static_cast<size_t>(rc);
    }
    return true;
}

bool RecvAll(socket_t sock, u8* data, size_t size) {
    size_t got = 0;
    while (got < size) {
        const int rc = static_cast<int>(
            ::recv(sock, reinterpret_cast<char*>(data + got), static_cast<int>(size - got), 0));
        if (rc <= 0) {
            return false;
        }
        got += static_cast<size_t>(rc);
    }
    return true;
}

bool SendFrame(socket_t sock, u8 kind, const std::string& payload) {
    std::vector<u8> frame(kFrameHeaderSize + payload.size());
    frame[0] = kind;
    const u32 length = htonl(static_cast<u32>(payload.size()));
    std::memcpy(frame.data() + 1, &length, sizeof(length));
    std::memcpy(frame.data() + kFrameHeaderSize, payload.data(), payload.size());
    return SendAll(sock, frame.data(), frame.size());
}

bool RecvFrame(socket_t sock, u8* out_kind, std::string* out_payload) {
    u8 header[kFrameHeaderSize];
    if (!RecvAll(sock, header, sizeof(header))) {
        return false;
    }
    u32 length = 0;
    std::memcpy(&length, header + 1, sizeof(length));
    length = ntohl(length);
    // A signaling blob is an SDP description at most; anything larger is a
    // desynchronised stream, not a big candidate.
    if (length > 64 * 1024) {
        return false;
    }
    out_payload->assign(length, '\0');
    if (length != 0 && !RecvAll(sock, reinterpret_cast<u8*>(out_payload->data()), length)) {
        return false;
    }
    *out_kind = header[0];
    return true;
}

// ---------------------------------------------------------------------------
// Candidate inspection
// ---------------------------------------------------------------------------

// "a=candidate:2 1 UDP 2116026111 <addr> <port> typ host" -> "host".
std::string CandidateType(const std::string& candidate) {
    const size_t at = candidate.find(" typ ");
    if (at == std::string::npos) {
        return "unknown";
    }
    const size_t start = at + 5;
    const size_t end = candidate.find(' ', start);
    return candidate.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

// The address of a candidate line, so an IPv6-only case can assert the family
// rather than trusting that configuring IPv6 produced an IPv6 connection.
std::string CandidateAddress(const std::string& candidate) {
    // Fields: candidate:<foundation> <component> <transport> <priority> <addr> <port> typ ...
    size_t pos = candidate.find("candidate:");
    if (pos == std::string::npos) {
        return {};
    }
    pos += 10;
    int field = 0;
    while (field < 4) {
        pos = candidate.find(' ', pos);
        if (pos == std::string::npos) {
            return {};
        }
        ++pos;
        ++field;
    }
    const size_t end = candidate.find(' ', pos);
    return candidate.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
}

bool IsIpv6(const std::string& address) {
    return address.find(':') != std::string::npos;
}

void SplitPair(const std::string& selected_path, std::string* local, std::string* remote) {
    const size_t bar = selected_path.find(" | ");
    if (bar == std::string::npos) {
        *local = selected_path;
        remote->clear();
        return;
    }
    *local = selected_path.substr(0, bar);
    *remote = selected_path.substr(bar + 3);
}

// ---------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------

struct Options {
    std::string role;
    std::string broker_host = "127.0.0.1";
    u16 broker_port = 0;
    std::string stun_host;
    u16 stun_port = 3478;
    std::string turn_host;
    u16 turn_port = 3478;
    std::string turn_user;
    std::string turn_pass;
    std::string expect_local;
    std::string expect_remote;
    std::string expect_family; // "ipv4", "ipv6", or empty
    bool expect_relayed = false;
    int timeout_seconds = 30;
    // libjuice verbosity. A case that nominates no pair at all says
    // nothing about why at the default level, and "connecting" is the
    // least informative state there is.
    std::string juice_log = "warn";
};

bool ParseHostPort(const std::string& text, std::string* host, u16* port) {
    // Accepts "host:port" and bare "host". An IPv6 literal must be bracketed,
    // otherwise its own colons are indistinguishable from the separator.
    if (!text.empty() && text.front() == '[') {
        const size_t close = text.find(']');
        if (close == std::string::npos) {
            return false;
        }
        *host = text.substr(1, close - 1);
        if (close + 1 < text.size() && text[close + 1] == ':') {
            *port = static_cast<u16>(std::atoi(text.c_str() + close + 2));
        }
        return true;
    }
    const size_t colon = text.rfind(':');
    if (colon == std::string::npos) {
        *host = text;
        return true;
    }
    *host = text.substr(0, colon);
    *port = static_cast<u16>(std::atoi(text.c_str() + colon + 1));
    return true;
}

void PrintUsage() {
    std::printf("usage: peer_netns_agent --role broker|offerer|answerer [options]\n"
                "\n"
                "  --broker HOST:PORT     rendezvous address (broker: the port to listen on)\n"
                "  --stun HOST[:PORT]     STUN server offered to the ICE agent\n"
                "  --turn HOST[:PORT]     TURN relay offered to the ICE agent\n"
                "  --turn-user USER       TURN username (coturn use-auth-secret form)\n"
                "  --turn-pass PASS       TURN credential\n"
                "  --expect-local TYPE    require this candidate type on the local side\n"
                "  --expect-remote TYPE   require this candidate type on the remote side\n"
                "  --expect-family FAM    require ipv4 or ipv6 on the nominated pair\n"
                "  --expect-relayed       require either side of the pair to be a relay\n"
                "  --timeout SECONDS      how long to wait for a connection (default 30)\n"
                "  --juice-log LEVEL      libjuice verbosity: none, warn, info, debug, verbose\n");
}

bool ParseOptions(int argc, char** argv, Options* out) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                return {};
            }
            return argv[++i];
        };
        if (arg == "--role") {
            out->role = next();
        } else if (arg == "--broker") {
            if (!ParseHostPort(next(), &out->broker_host, &out->broker_port)) {
                return false;
            }
        } else if (arg == "--stun") {
            if (!ParseHostPort(next(), &out->stun_host, &out->stun_port)) {
                return false;
            }
        } else if (arg == "--turn") {
            if (!ParseHostPort(next(), &out->turn_host, &out->turn_port)) {
                return false;
            }
        } else if (arg == "--turn-user") {
            out->turn_user = next();
        } else if (arg == "--turn-pass") {
            out->turn_pass = next();
        } else if (arg == "--expect-local") {
            out->expect_local = next();
        } else if (arg == "--expect-remote") {
            out->expect_remote = next();
        } else if (arg == "--expect-family") {
            out->expect_family = next();
        } else if (arg == "--expect-relayed") {
            out->expect_relayed = true;
        } else if (arg == "--juice-log") {
            out->juice_log = next();
        } else if (arg == "--timeout") {
            out->timeout_seconds = std::atoi(next().c_str());
        } else if (arg == "--help" || arg == "-h") {
            return false;
        } else {
            std::printf("unknown argument: %s\n", arg.c_str());
            return false;
        }
    }
    if (out->role.empty() || out->broker_port == 0) {
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Broker
// ---------------------------------------------------------------------------

// Accepts exactly two agents and forwards every frame from each to the other,
// unread. It never inspects a payload, which is also true of the server it
// stands in for.
int RunBroker(const Options& options) {
    // AF_INET6 with V6ONLY off, so one listener serves both families. A
    // case that runs the peers on IPv6 still needs its signaling to arrive.
    socket_t listener = ::socket(AF_INET6, SOCK_STREAM, 0);
    if (listener == INVALID_SOCK) {
        std::printf("broker: socket failed\n");
        return 1;
    }
    int v6only = 0;
    ::setsockopt(listener, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&v6only),
                 sizeof(v6only));
    int reuse = 1;
    ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
                 sizeof(reuse));

    sockaddr_in6 addr{};
    addr.sin6_family = AF_INET6;
    addr.sin6_addr = in6addr_any;
    addr.sin6_port = htons(options.broker_port);
    if (::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(listener, 4) != 0) {
        std::printf("broker: bind/listen on port %u failed\n", options.broker_port);
        CLOSE_SOCKET(listener);
        return 1;
    }
    std::printf("broker: listening on %u\n", options.broker_port);
    std::fflush(stdout);

    socket_t peers[2] = {INVALID_SOCK, INVALID_SOCK};
    for (int i = 0; i < 2; ++i) {
        peers[i] = ::accept(listener, nullptr, nullptr);
        if (peers[i] == INVALID_SOCK) {
            std::printf("broker: accept failed\n");
            CLOSE_SOCKET(listener);
            return 1;
        }
        int nodelay = 1;
        ::setsockopt(peers[i], IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay),
                     sizeof(nodelay));
        std::printf("broker: agent %d connected\n", i);
        std::fflush(stdout);
    }
    CLOSE_SOCKET(listener);

    const auto pump = [&](int from, int to) {
        u8 kind = 0;
        std::string payload;
        while (RecvFrame(peers[from], &kind, &payload)) {
            if (!SendFrame(peers[to], kind, payload)) {
                break;
            }
        }
        // Closing the far side unblocks the other pump, so the broker exits
        // when either agent is done rather than hanging until the harness
        // kills it.
        ::shutdown(peers[to], 1 /* SHUT_WR */);
    };

    std::thread forward_a([&] { pump(0, 1); });
    std::thread forward_b([&] { pump(1, 0); });
    forward_a.join();
    forward_b.join();

    CLOSE_SOCKET(peers[0]);
    CLOSE_SOCKET(peers[1]);
    std::printf("broker: done\n");
    return 0;
}

// ---------------------------------------------------------------------------
// Agent
// ---------------------------------------------------------------------------

socket_t ConnectToBroker(const Options& options) {
    // getaddrinfo rather than inet_pton: the broker address is IPv4 in most
    // cases and IPv6 in the cases that exist to prove the path stayed IPv6.
    char port_text[16];
    std::snprintf(port_text, sizeof(port_text), "%u", options.broker_port);

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    // The broker may not be listening the instant this process starts; the
    // harness launches all three at once. Retry briefly rather than making
    // every script sleep an arbitrary amount first.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (std::chrono::steady_clock::now() < deadline) {
        addrinfo* results = nullptr;
        if (::getaddrinfo(options.broker_host.c_str(), port_text, &hints, &results) == 0) {
            for (addrinfo* it = results; it != nullptr; it = it->ai_next) {
                socket_t sock = ::socket(it->ai_family, it->ai_socktype, it->ai_protocol);
                if (sock == INVALID_SOCK) {
                    continue;
                }
                if (::connect(sock, it->ai_addr, static_cast<int>(it->ai_addrlen)) == 0) {
                    int nodelay = 1;
                    ::setsockopt(sock, IPPROTO_TCP, TCP_NODELAY,
                                 reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
                    ::freeaddrinfo(results);
                    return sock;
                }
                CLOSE_SOCKET(sock);
            }
            ::freeaddrinfo(results);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return INVALID_SOCK;
}

std::vector<IceServerEntry> BuildIceServers(const Options& options) {
    std::vector<IceServerEntry> servers;
    if (!options.stun_host.empty()) {
        IceServerEntry stun;
        stun.host = options.stun_host;
        stun.port = options.stun_port;
        stun.is_turn = false;
        servers.push_back(stun);
    }
    if (!options.turn_host.empty()) {
        IceServerEntry turn;
        turn.host = options.turn_host;
        turn.port = options.turn_port;
        turn.is_turn = true;
        turn.username = options.turn_user;
        turn.credential = options.turn_pass;
        servers.push_back(turn);
    }
    return servers;
}

int RunAgent(const Options& options) {
    const bool is_offerer = options.role == "offerer";

    socket_t broker = ConnectToBroker(options);
    if (broker == INVALID_SOCK) {
        std::printf("RESULT role=%s status=no-broker\n", options.role.c_str());
        return 1;
    }

    std::mutex received_mutex;
    std::condition_variable received_cv;
    std::vector<std::pair<u32, std::vector<u8>>> received;

    PeerConnection::Params params;
    params.session_id = 1;
    params.generation = 1;
    params.is_offerer = is_offerer;
    params.peer_npid = is_offerer ? "answerer" : "offerer";
    params.peer_virtual_addr_nbo = htonl(is_offerer ? kAnswererVirtualAddr : kOffererVirtualAddr);

    const std::vector<IceServerEntry> ice_servers = BuildIceServers(options);

    PeerConnection connection(
        params, ice_servers,
        [broker](PeerSignalKind kind, const std::string& payload) {
            SendFrame(broker, static_cast<u8>(kind), payload);
        },
        [&](u32 from, const u8* data, size_t size) {
            {
                std::lock_guard lock(received_mutex);
                received.emplace_back(from, std::vector<u8>(data, data + size));
            }
            received_cv.notify_all();
        });

    std::atomic<bool> reader_running{true};
    std::thread reader([&] {
        u8 kind = 0;
        std::string payload;
        while (reader_running.load() && RecvFrame(broker, &kind, &payload)) {
            switch (static_cast<PeerSignalKind>(kind)) {
            case PeerSignalKind::Description:
                connection.OnRemoteDescription(payload);
                break;
            case PeerSignalKind::Candidate:
                connection.OnRemoteCandidate(payload);
                break;
            case PeerSignalKind::GatheringDone:
                connection.OnRemoteGatheringDone();
                break;
            }
        }
    });

    if (!connection.Start()) {
        std::printf("RESULT role=%s status=start-failed\n", options.role.c_str());
        reader_running.store(false);
        CLOSE_SOCKET(broker);
        reader.join();
        return 1;
    }

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(options.timeout_seconds);
    while (std::chrono::steady_clock::now() < deadline &&
           connection.State() != PeerTransportState::Connected &&
           connection.State() != PeerTransportState::Failed) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    int failures = 0;
    const PeerTransportState state = connection.State();
    if (state != PeerTransportState::Connected) {
        std::printf("RESULT role=%s status=not-connected state=%s\n", options.role.c_str(),
                    ShadNet::PeerTransportStateName(state));
        ++failures;
    }

    std::string local_candidate;
    std::string remote_candidate;
    if (state == PeerTransportState::Connected) {
        SplitPair(connection.SelectedPath(), &local_candidate, &remote_candidate);

        // A 1200-byte payload, because a datagram that only proves the path
        // exists at 4 bytes says nothing about whether it survives at the size
        // the game actually sends.
        std::vector<u8> payload(1200);
        for (size_t i = 0; i < payload.size(); ++i) {
            payload[i] = static_cast<u8>(i * 7 + 3);
        }
        if (connection.Send(payload.data(), payload.size()) != static_cast<int>(payload.size())) {
            std::printf("RESULT role=%s status=send-failed\n", options.role.c_str());
            ++failures;
        }

        // Both sides send and both sides expect, so neither has to know which
        // one goes first.
        std::unique_lock lock(received_mutex);
        const bool got =
            received_cv.wait_for(lock, std::chrono::seconds(10), [&] { return !received.empty(); });
        if (!got) {
            std::printf("RESULT role=%s status=no-datagram\n", options.role.c_str());
            ++failures;
        } else {
            const u32 expected_from =
                htonl(is_offerer ? kAnswererVirtualAddr : kOffererVirtualAddr);
            if (received[0].first != expected_from) {
                std::printf("RESULT role=%s status=wrong-source-addr\n", options.role.c_str());
                ++failures;
            }
            if (received[0].second.size() != payload.size()) {
                std::printf("RESULT role=%s status=wrong-size got=%zu\n", options.role.c_str(),
                            received[0].second.size());
                ++failures;
            }
        }
    }

    const std::string local_type = CandidateType(local_candidate);
    const std::string remote_type = CandidateType(remote_candidate);
    const std::string local_address = CandidateAddress(local_candidate);

    std::printf("RESULT role=%s state=%s local_type=%s remote_type=%s family=%s setup_ms=%lld\n",
                options.role.c_str(), ShadNet::PeerTransportStateName(state), local_type.c_str(),
                remote_type.c_str(), IsIpv6(local_address) ? "ipv6" : "ipv4",
                static_cast<long long>(connection.SetupMillis()));
    std::printf("PATH  role=%s local=%s\n", options.role.c_str(), local_candidate.c_str());
    std::printf("PATH  role=%s remote=%s\n", options.role.c_str(), remote_candidate.c_str());

    if (!options.expect_local.empty() && local_type != options.expect_local) {
        std::printf("EXPECT-FAIL local_type=%s want=%s\n", local_type.c_str(),
                    options.expect_local.c_str());
        ++failures;
    }
    if (!options.expect_remote.empty() && remote_type != options.expect_remote) {
        std::printf("EXPECT-FAIL remote_type=%s want=%s\n", remote_type.c_str(),
                    options.expect_remote.c_str());
        ++failures;
    }
    // Which SIDE of the pair is the relay is not ours to decide: it depends
    // on whose allocation ICE nominates, and it genuinely differs run to
    // run. Sending to a peer's relay makes the local side peer-reflexive,
    // so a relayed connection can show relay on either end. What is being
    // asserted is that the path goes through the relay at all.
    if (options.expect_relayed && local_type != "relay" && remote_type != "relay") {
        std::printf("EXPECT-FAIL neither side relayed: local=%s remote=%s\n", local_type.c_str(),
                    remote_type.c_str());
        ++failures;
    }
    if (!options.expect_family.empty()) {
        const std::string family = IsIpv6(local_address) ? "ipv6" : "ipv4";
        if (family != options.expect_family) {
            std::printf("EXPECT-FAIL family=%s want=%s\n", family.c_str(),
                        options.expect_family.c_str());
            ++failures;
        }
    }
    // Two controlling agents can still reach a connection, so this is checked
    // even when everything above passed.
    if (g_role_conflict.load()) {
        std::printf("EXPECT-FAIL role-conflict observed\n");
        ++failures;
    }

    reader_running.store(false);
    ::shutdown(broker, SHUT_RDWR);
    CLOSE_SOCKET(broker);
    reader.join();

    std::printf(failures == 0 ? "PASS role=%s\n" : "FAIL role=%s\n", options.role.c_str());
    return failures == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    if (!ParseOptions(argc, argv, &options)) {
        PrintUsage();
        return 2;
    }
    if (!SocketStartup()) {
        std::printf("socket startup failed\n");
        return 1;
    }

    g_juice_print_level = JuiceLogLevel(options.juice_log);
    juice_set_log_handler(&OnJuiceLog);
    juice_set_log_level(g_juice_print_level);

    // Unbuffered, so a namespace that is about to be torn down does not take
    // the explanation of the failure with it.
    ::setvbuf(stdout, nullptr, _IONBF, 0);

    if (options.role == "broker") {
        return RunBroker(options);
    }
    if (options.role == "offerer" || options.role == "answerer") {
        return RunAgent(options);
    }
    PrintUsage();
    return 2;
}
