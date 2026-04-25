#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class CVoIPServer {
public:
    static CVoIPServer& instance();

    CVoIPServer(const CVoIPServer&) = delete;
    CVoIPServer& operator=(const CVoIPServer&) = delete;

    int run();
    void requestStop();

private:
    struct ClientSession {
        SOCKET      sock = INVALID_SOCKET;
        std::string playerId;
        std::string channelId;
        uint16_t    localUdpPort = 0;
        std::string lanIp;
        std::string publicIp;
        uint16_t    publicPort = 0;
        uint32_t    ssrc = 0;

        std::mutex  udpAddrMtx;
        std::string udpExtIp;
        uint16_t    udpExtPort = 0;
        bool        udpAddrKnown = false;

        std::atomic<bool> alive{true};
        std::chrono::steady_clock::time_point lastPong;
        std::mutex sendMtx;

        uint16_t relayTargetPort() const;
        const std::string& relayTargetIp() const;
        bool sendLine(const std::string& msg);
    };

    CVoIPServer();
    ~CVoIPServer();

    void runSignalingServer();
    void runTurnRelay();
    void runHeartbeat();
    void handleClient(SOCKET clientSock, std::string peerIp);
    void cleanupSessions();

    std::vector<std::shared_ptr<ClientSession>>
        getChannelPeers(const std::string& channelId,
                        const std::string& excludePlayerId = "");
    void broadcastToChannel(const std::string& channelId,
                            const std::string& excludePlayerId,
                            const std::string& msg);
    void broadcastPeerAddr(const std::shared_ptr<ClientSession>& session,
                           const std::string& excludePlayerId = "");

    static std::string jStr(const std::string& j, const std::string& key);
    static int jInt(const std::string& j,
                    const std::string& key,
                    int def = 0);
    static bool isJsonRawValue(const std::string& v);
    static std::string jBuild(
        const std::vector<std::pair<std::string, std::string>>& fields);
    static bool parseRtpSsrc(const uint8_t* data, int len, uint32_t& ssrcOut);

    static BOOL WINAPI consoleCtrlHandler(DWORD);

    static constexpr uint16_t SIGNALING_PORT = 7000;
    static constexpr uint16_t RELAY_PORT = 40000;
    static constexpr int PING_INTERVAL_MS = 30000;
    static constexpr int PONG_TIMEOUT_MS = 60000;

    std::atomic<bool> m_running{true};
    std::mutex m_sessionsMtx;
    std::map<std::string, std::shared_ptr<ClientSession>> m_sessions;
};
