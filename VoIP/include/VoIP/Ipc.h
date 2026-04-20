#pragma once
// ============================================================
//  Ipc.h  –  VoIP.exe <-> 遊戲 Client 的本機 IPC 通訊
//  協定：JSON over TCP（127.0.0.1），含版本號欄位
// ============================================================
#include <string>
#include <functional>
#include <cstdint>

namespace VoIP {

// IPC 訊息格式（JSON 欄位名稱）
// { "ver": 1, "cmd": "JOIN", "channelId": "...", "token": "..." }
// { "ver": 1, "cmd": "LEAVE" }
// { "ver": 1, "cmd": "MUTE", "muted": true }
// { "ver": 1, "evt": "SPEAKING", "playerId": "...", "speaking": true }
// { "ver": 1, "evt": "PEER_JOIN", "playerId": "..." }
// { "ver": 1, "evt": "PEER_LEAVE", "playerId": "..." }
// { "ver": 1, "evt": "ERROR", "message": "..." }

constexpr int IPC_VERSION = 1;
constexpr uint16_t IPC_PORT = 17832; // 本機固定 Port

using IpcMessageCallback = std::function<void(const std::string& jsonMsg)>;

// ── IPC Server（VoIP.exe 端，監聽遊戲 Client 指令）──
class IpcServer {
public:
    IpcServer();
    ~IpcServer();

    bool start(IpcMessageCallback onMessage);
    void stop();

    // 主動推送事件給遊戲 Client
    void send(const std::string& jsonMsg);

private:
    struct Impl;
    Impl* m_impl = nullptr;
};

// ── IPC Client（遊戲 Client 端，發送指令給 VoIP.exe）──
class IpcClient {
public:
    IpcClient();
    ~IpcClient();

    bool connect(IpcMessageCallback onEvent);
    void disconnect();

    void send(const std::string& jsonMsg);

private:
    struct Impl;
    Impl* m_impl = nullptr;
};

} // namespace VoIP
