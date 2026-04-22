#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace VoIP {

struct PeerInfo {
    std::string playerId;
    std::string publicIp;
    uint16_t    publicPort = 0;
};

struct ChannelConfig {
    std::string signalingServer;
    uint16_t    signalingPort = 7000;
    std::string turnServer;
    uint16_t    turnPort = 3478;
    std::string token;
    std::string channelId;
    std::string playerId;

    // Audio tuning
    int         opusBitrate = 32000;
    float       captureVadDb = -50.0f;
    int         captureHangoverFrames = 12;

    // Echo cancellation tuning
    bool        enableEchoCancel = true;
    int         aecFilterMs = 200;
    int         aecEchoSuppress = -40;
    int         aecEchoSuppressActive = -15;

    // Denoise
    bool        enableDenoise = true;
};

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

    bool join(const ChannelConfig& cfg, const ChannelEvents& events);
    void leave();

    void setMuted(bool muted);
    bool isMuted() const;

    std::vector<PeerInfo> peers() const;

    void tick();

private:
    struct Impl;
    Impl* m_impl = nullptr;
};

} // namespace VoIP
