#pragma once
// ============================================================
//  Channel.h  –  語音頻道與連線工作階段管理
//  負責：建立 P2P 連線、TURN 中繼切換、成員管理
// ============================================================
#include <string>
#include <vector>
#include <functional>
#include <cstdint>

namespace VoIP {

struct PeerInfo {
    std::string playerId;
    std::string publicIp;
    uint16_t    publicPort = 0;
};

struct ChannelConfig {
    std::string signalingServer;  // Signaling Server 位址
    uint16_t    signalingPort = 7000;
    std::string turnServer;       // TURN Server 位址（通常同 signaling）
    uint16_t    turnPort = 3478;
    std::string token;            // 短效 JWT Token（由遊戲 Server 產生）
    std::string channelId;        // 頻道識別碼
    std::string playerId;         // 本機玩家識別碼
};

// 頻道事件回呼
struct ChannelEvents {
    std::function<void(const std::string& playerId)> onPeerJoined;
    std::function<void(const std::string& playerId)> onPeerLeft;
    std::function<void(const std::string& playerId, bool speaking)> onSpeakingChanged;
    std::function<void(const std::string& error)> onError;
};

class Channel {
public:
    Channel();
    ~Channel();

    // 加入頻道（非同步，事件透過 ChannelEvents 回呼）
    bool join(const ChannelConfig& cfg, const ChannelEvents& events);
    void leave();

    // 靜音控制
    void setMuted(bool muted);
    bool isMuted() const;

    // 查詢成員列表
    std::vector<PeerInfo> peers() const;

    // 心跳（定期呼叫，確認連線存活）
    void tick();

private:
    struct Impl;
    Impl* m_impl = nullptr;
};

} // namespace VoIP
