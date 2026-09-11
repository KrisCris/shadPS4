// SPDX-FileCopyrightText: Copyright 2019-2026 rpcs3 Project
// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <semaphore>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include "common/types.h"
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using ShadSocketHandle = SOCKET;
static constexpr ShadSocketHandle SHAD_INVALID_SOCK = INVALID_SOCKET;
#define SHAD_CLOSE(s) ::closesocket(s)
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using ShadSocketHandle = int;
static constexpr ShadSocketHandle SHAD_INVALID_SOCK = -1;
#define SHAD_CLOSE(s) ::close(s)
#endif

namespace ShadNet {

// Protocol constants
static constexpr u32 SHAD_HEADER_SIZE = 15;
static constexpr u32 SHAD_CONNECT_TIMEOUT_MS = 10000; // 10 second connect/handshake timeout
// MAX_ATTEMPTS * CONNECT_TIMEOUT + sum(backoffs).
static constexpr u32 SHAD_CONNECT_MAX_ATTEMPTS = 4;
static constexpr u32 SHAD_CONNECT_RETRY_BACKOFF_MS =
    1000; // base backoff, doubled per retry (cap 8s)
static constexpr u32 SHAD_PROTOCOL_VERSION = 2;
static constexpr u32 SHAD_MAX_PACKET_SIZE = 0x800000; // 8 MiB

// Protocol enumerations (must match shadnet server protocol.h)
enum class PacketType : u8 {
    Request = 0,
    Reply = 1,
    Notification = 2,
    ServerInfo = 3,
};

enum class CommandType : u16 {
    Login = 0,
    Terminate = 1,
    Create = 2,
    Delete = 3,
    SendToken = 4,
    SendResetToken = 5,
    ResetPassword = 6,
    ResetState = 7,
    AddFriend = 8,
    RemoveFriend = 9,
    AddBlock = 10,
    RemoveBlock = 11,
    GetServerFeatures = 12,
    SetClientVersion = 13,
    GetBoardInfos = 30,
    RecordScore = 31,
    RecordScoreData = 32,
    GetScoreData = 33,
    GetScoreRange = 34,
    GetScoreFriends = 35,
    GetScoreNpid = 36,
    GetScoreAccountId = 37,
    GetScoreGameDataByAccId = 38,
    GetToken = 39,
    SetAppearOffline = 40,
    LookupOnlineId = 41,
    // Matchmaking
    ContextStart = 100,
    CreateRoom = 101,
    JoinRoom = 102,
    LeaveRoom = 103,
    SearchRoom = 104,
    // 105 was RequestSignalingInfos, retired with the STUN address
    // registry it read from. Peers are reached through the peer-session
    // commands instead. The number is not reused.
    ContextStop = 106,
    SetUserInfo = 107,
    SetRoomDataInternal = 108,
    SetRoomDataExternal = 109,
    KickoutRoomMember = 110,
    GetWorldInfoList = 111,
    GetRoomDataExternalList = 112,
    GetUserInfoList = 113,
    GetRoomMemberDataExternalList = 114,
    SendRoomMessage = 115,
    // Peer connectivity (protocol v2)
    PeerSessionBegin = 120,
    PeerSignal = 121,
    PeerSessionEnd = 122,
    GetIceServers = 123,
    // Title User Storage (TUS)
    TusSetData = 201,
    TusGetData = 202,
    TusSetMultiSlotVariable = 203,
    TusGetMultiSlotVariable = 204,
    TusAddAndGetVariable = 205,
    TusGetMultiSlotDataStatus = 206,
    TusGetMultiUserDataStatus = 207,
    TusGetFriendsDataStatus = 208,
    TusDeleteMultiSlotData = 209,
    TusGetMultiUserVariable = 210,
    TusTryAndSetVariable = 211,
    TusGetFriendsVariable = 212,
    TusDeleteMultiSlotVariable = 213,
    TssGetData = 214,
    // Trophies
    UnlockTrophy = 301,
    SyncTrophies = 302,
};

enum class NotificationType : u16 {
    FriendQuery = 5,
    FriendNew = 6,
    FriendLost = 7,
    FriendStatus = 8,
    RoomEvent = 10,
    RoomMessage = 11,
    WebApiPushEvent = 17, // Generic NP WebApi push event
    // Peer connectivity (protocol v2). PeerSignal carries an opaque ICE
    // description or candidate from the other participant of a session.
    PeerSessionOpened = 20,
    PeerSignal = 21,
    PeerSessionClosed = 22,
};

enum class ErrorType : uint8_t {
    NoError = 0,
    Malformed = 1,
    Invalid = 2,
    InvalidInput = 3,
    TooSoon = 4,
    LoginError = 5,
    LoginAlreadyLoggedIn = 6,
    LoginInvalidUsername = 7,
    LoginInvalidPassword = 8,
    LoginInvalidToken = 9,
    CreationError = 10,
    CreationExistingUsername = 11,
    CreationBannedEmailProvider = 12,
    CreationExistingEmail = 13,
    RoomMissing = 14,
    RoomAlreadyJoined = 15,
    RoomFull = 16,
    RoomPasswordMismatch = 17,
    RoomPasswordMissing = 18,
    RoomGroupNoJoinLabel = 19,
    RoomGroupFull = 20,
    RoomGroupJoinLabelNotFound = 21,
    RoomGroupMaxSlotMismatch = 22,
    Unauthorized = 23,
    DbFail = 24,
    EmailFail = 25,
    NotFound = 26,
    Blocked = 27,
    AlreadyFriend = 28,
    ScoreNotBest = 29,
    ScoreInvalid = 30,
    ScoreHasData = 31,
    CondFail = 32,
    Unsupported = 33,
    TooLarge = 34, // TSS file on the server exceeds the slot's documented maximum
};

enum class ShadNetState {
    Ok,
    FailureInput,
    FailureResolve,
    FailureConnect,
    FailureServerInfo,
    FailureAuth,
    FailureAlreadyIn,
    FailureUsername,
    FailurePassword,
    FailureToken,
    FailureProtocol,
    FailureOther,
};

// Callback data structures

struct FriendEntry {
    std::string npid;
    bool online = false;
};

struct LoginResult {
    ErrorType error = ErrorType::Malformed;
    std::string avatarUrl;
    u64 userId = 0;
    std::vector<FriendEntry> friends;
    std::vector<std::string> requestsSent;
    std::vector<std::string> requestsReceived;
    std::vector<std::string> blocked;
};

struct NotifyFriendQuery {
    std::string fromNpid;
};
struct NotifyFriendNew {
    std::string npid;
    bool online = false;
};
struct NotifyFriendLost {
    std::string npid;
};
struct NotifyFriendStatus {
    std::string npid;
    bool online = false;
    u64 timestamp = 0;
};
struct NotifyWebApiPushEvent {
    std::string npServiceName; // may be empty (catch-all listeners match any)
    u32 npServiceLabel = 0;
    std::string dataType; // e.g. "np:service:..."
    std::string data;     // raw event body (typically PSN-format JSON)
    std::string fromNpid; // may be empty
    std::string toNpid;   // may be empty
    // Optional extended-data (key,value) pairs (e.g. friendlist trigger/additionalTrigger,
    // presence gameStatus/gameData). Empty when the server sends none / is older.
    std::vector<std::pair<std::string, std::string>> extdData;
};

// Peer connectivity (protocol v2)

enum class PeerSignalKind : u32 {
    Description = 0,   // full local ICE description (SDP)
    Candidate = 1,     // one trickled candidate
    GatheringDone = 2, // payload is empty
};

// NotificationType::PeerSessionOpened (20)
struct NotifyPeerSessionOpened {
    u64 session_id = 0;
    u32 generation = 0;
    // Exactly one participant offers, chosen by the server so both sides agree
    // no matter who began first.
    bool is_offerer = false;
    std::string peer_npid;
    // Host byte order, inside 198.18.0.0/15. Converted to network byte order
    // once, where the transport records them; see PeerAddressTable.
    u32 local_virtual_addr = 0;
    u32 peer_virtual_addr = 0;
    std::string title_id;
};

// NotificationType::PeerSignal (21)
struct NotifyPeerSignal {
    u64 session_id = 0;
    u32 generation = 0;
    PeerSignalKind kind = PeerSignalKind::Description;
    std::string payload;
    // Filled by the server from the sender's authenticated connection, so it
    // cannot be spoofed by the sender.
    std::string from_npid;
};

// NotificationType::PeerSessionClosed (22)
struct NotifyPeerSessionClosed {
    u64 session_id = 0;
    u32 generation = 0;
    u32 reason = 0; // 0 cancelled, 1 connected, 2 failed, 3 logout
};

struct IceServerEntry {
    std::string host;
    u16 port = 0;
    bool is_turn = false;
    std::string username;   // empty for plain STUN
    std::string credential; // empty for plain STUN
    u64 expires_at = 0;     // unix seconds; 0 when not applicable
};

struct MatchingBinAttr {
    u32 attr_id = 0;
    u64 update_date = 0;
    u32 update_member_id = 0;
    std::vector<u8> data;
};

struct NotifyRoomEvent {
    u32 ctx_id = 0;
    u64 room_id = 0;
    u32 event = 0;
    u32 event_cause = 0;
    s32 error_code = 0;
    u32 flags = 0;
    bool has_passwd_mask = false;
    u64 passwd_slot_mask = 0;

    std::string member_npid;
    u64 member_account_id = 0;
    u32 member_platform = 0;
    u32 member_id = 0;
    u32 member_team_id = 0;
    bool member_is_owner = false;
    u64 member_join_date = 0;
    u32 member_nat_type = 0;
    u32 member_flag_attr = 0;
    u32 member_group_id = 0;
    std::string member_addr;
    u32 member_port = 0;
    std::vector<MatchingBinAttr> member_bin_attrs;

    std::vector<MatchingBinAttr> bin_attrs;
};

struct NotifyRoomMessage {
    u32 ctx_id = 0;
    u64 room_id = 0;
    u32 src_member_id = 0;
    u32 event = 0;
    u32 cast_type = 0;
    std::vector<u32> dst_member_ids;
    std::string src_npid;
    u64 src_account_id = 0;
    u32 src_platform = 0;
    std::vector<u8> msg;
};

// ShadNetClient

class ShadNetClient {
public:
    ShadNetClient();
    ~ShadNetClient();
    ShadNetClient(const ShadNetClient&) = delete;
    ShadNetClient& operator=(const ShadNetClient&) = delete;

    void Start(const std::string& host, u16 port, const std::string& npid,
               const std::string& password, const std::string& token = {});
    void Stop();

    ShadNetState WaitForConnection();
    ShadNetState WaitForAuthenticated();

    bool IsConnected() const;
    bool IsAuthenticated() const;
    ShadNetState GetState() const;

    const std::string& GetAvatarUrl() const;
    u64 GetUserId() const;
    // Online ID this client logged in with (set once in Start()).
    const std::string& GetNpid() const;
    u32 GetServerProtocolVersion() const {
        return m_server_protocol_version.load();
    }
    u32 GetAddrLocal() const;
    u32 GetAddrServer() const;
    bool IsMatching2Enabled() const;
    bool IsTrophiesEnabled() const;
    u64 ReportClientVersion();
    static std::string BuildVersionString();

    u64 UnlockTrophy(const std::string& com_id, s32 trophy_id, u64 timestamp);
    u64 SyncTrophies(const std::string& com_id,
                     const std::vector<std::pair<s32, u64>>& local_trophies);
    u32 GetNumFriends() const;
    std::optional<std::string> GetFriendNpid(u32 index) const;

    std::string GetBearerToken() const;

    // Callbacks
    std::function<void(const LoginResult&)> onLoginResult;
    std::function<void(const NotifyFriendQuery&)> onFriendQuery;
    std::function<void(const NotifyFriendNew&)> onFriendNew;
    std::function<void(const NotifyFriendLost&)> onFriendLost;
    std::function<void(const NotifyFriendStatus&)> onFriendStatus;
    std::function<void(const NotifyRoomEvent&)> onRoomEvent;
    std::function<void(const NotifyRoomMessage&)> onRoomMessage;
    std::function<void(const NotifyWebApiPushEvent&)> onWebApiPushEvent;
    std::function<void(const NotifyPeerSessionOpened&)> onPeerSessionOpened;
    std::function<void(const NotifyPeerSignal&)> onPeerSignal;
    std::function<void(const NotifyPeerSessionClosed&)> onPeerSessionClosed;
    // Async reply callback.
    //   cmd    —command this reply is for (matches the request's cmd)
    //   pkt_id —packet id echoed back from the original request header
    //   error  —ErrorType byte that prefixes every reply body
    //   body   —reply payload AFTER the error byte (may be empty)
    std::function<void(CommandType cmd, u64 pkt_id, ErrorType error, const std::vector<u8>& body)>
        onAsyncReply;

    // Submit a Request packet for async processing.
    // Allocates a packet id, builds the packet, pushes it to the writer queue.
    // Returns the packet id so callers can correlate the eventual reply.
    u64 SubmitRequest(CommandType cmd, const std::vector<u8>& payload);

    // Friend / block commands.
    u64 AddFriend(const std::string& npid);
    u64 RemoveFriend(const std::string& npid);
    u64 AddBlock(const std::string& npid);
    u64 RemoveBlock(const std::string& npid);
    // Set the Appear-Offline preference mid-session (server handles us as offline while set).
    u64 SetAppearOffline(bool enable);
    // Global online ID to account ID resolution
    u64 LookupOnlineId(const std::string& npid);

    // Peer connectivity (protocol v2). Each returns the packet id, so the
    // caller can correlate the reply through onAsyncReply.
    u64 PeerSessionBegin(const std::string& target_npid, const std::string& title_id, u32 attempt);
    u64 PeerSignal(u64 session_id, u32 generation, PeerSignalKind kind, const std::string& payload);
    u64 PeerSessionEnd(u64 session_id, u32 generation, u32 reason);
    u64 GetIceServers();

    // Decodes a GetIceServers reply body (the bytes after the ErrorType byte).
    static std::vector<IceServerEntry> ParseIceServersReply(const std::vector<u8>& body);
    // Decodes a PeerSessionBegin reply body the same way.
    static bool ParsePeerSessionBeginReply(const std::vector<u8>& body,
                                           NotifyPeerSessionOpened* out);

private:
    void ConnectThread();
    void ReaderThread();
    void WriterThread();
    bool DoConnect();
    void DoDisconnect();
    bool RecvN(u8* buf, u32 n);
    bool SendAll(const std::vector<u8>& data);
    std::vector<u8> BuildPacket(CommandType cmd, u64 id, const std::vector<u8>& payload) const;
    void DispatchPacket(PacketType type, u16 cmd_raw, u64 pkt_id, const std::vector<u8>& payload);
    void HandleLoginReply(const std::vector<u8>& payload);
    void HandleGetTokenReply(const std::vector<u8>& payload);
    void HandleServerFeaturesReply(const std::vector<u8>& payload);
    void HandleNotification(u16 cmd_raw, const std::vector<u8>& payload);
    bool RequestServerFeatures();

    // Helper: read a u32-LE-prefixed proto blob from a byte vector at pos.
    static std::string ExtractBlob(const std::vector<u8>& p, int pos);

    static void PutLE16(std::vector<u8>& b, size_t off, u16 v);
    static void PutLE32(std::vector<u8>& b, size_t off, u32 v);
    static void PutLE64(std::vector<u8>& b, size_t off, u64 v);
    static u16 GetLE16(const u8* p);
    static u32 GetLE32(const u8* p);
    static u64 GetLE64(const u8* p);

    ShadSocketHandle m_sock = SHAD_INVALID_SOCK;
    std::string m_host;
    u16 m_port = 31313;
    std::string m_npid;
    std::string m_password;
    std::string m_token;
    bool m_appear_offline = false; // user's Appear-Offline preference (sent at login)

    std::atomic<bool> m_terminate{false};
    std::atomic<bool> m_connected{false};
    std::atomic<bool> m_authenticated{false};
    std::atomic<ShadNetState> m_state{ShadNetState::Ok};

    std::binary_semaphore m_sem_connected{0};
    std::binary_semaphore m_sem_authenticated{0};
    std::mutex m_mutex_connected;
    std::mutex m_mutex_authenticated;

    std::thread m_thread_connect;
    std::thread m_thread_reader;
    std::thread m_thread_writer;

    std::mutex m_mutex_send_direct;
    std::mutex m_mutex_send_queue;
    std::condition_variable m_cv_send_queue;
    std::vector<std::vector<u8>> m_send_queue;

    std::string m_avatar_url;
    u64 m_user_id = 0;
    mutable std::mutex m_mutex_bearer;
    std::string m_bearer_token;
    std::atomic<u32> m_addr_local{0};
    std::atomic<u32> m_addr_server{0};
    std::atomic<u32> m_server_protocol_version{0};
    std::atomic<bool> m_matching2_enabled{false};
    std::atomic<bool> m_trophies_enabled{false};
    std::atomic<bool> m_server_features_received{false};

    mutable std::mutex m_mutex_friends;
    std::vector<FriendEntry> m_friends;

    std::atomic<u64> m_pkt_counter{1};
};

} // namespace ShadNet
