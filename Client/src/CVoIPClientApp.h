#pragma once

#include "VoIP/VoIP.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

struct CVoIPClientAppOptions {
    bool ipcMode = false;
    bool autoJoinOnStart = false;
    int ipcHeartbeatTimeoutMs = 10000;
    int ipcShutdownGraceMs = 5000;
    bool forceExitOnIpcShutdownTimeout = true;

#ifdef _DEBUG
    bool printSpeakingDebug = true;
#else
    bool printSpeakingDebug = false;
#endif

    VoIP::IpcOptions ipcOptions;
    VoIP::ChannelConfig initialConfig;
};

class CVoIPClientApp {
public:
    CVoIPClientApp();
    ~CVoIPClientApp();

    CVoIPClientApp(const CVoIPClientApp&) = delete;
    CVoIPClientApp& operator=(const CVoIPClientApp&) = delete;

    bool start(const CVoIPClientAppOptions& options);
    void stop();
    void tick();
    void requestQuit();

    bool isRunning() const;
    bool isIpcMode() const;
    bool isJoined() const;
    bool isMuted() const;

    bool joinCurrentConfig(const std::string& reason);
    void leaveChannel(const std::string& reason = {});
    void setMuted(bool muted);
    void toggleMuted();
    void applyLogin(const std::string& playerId,
                    const std::string& token,
                    const std::string& server,
                    const std::string& channelId);

    VoIP::ChannelConfig snapshotConfig() const;

    static bool parseIpcType(const std::string& text, VoIP::IpcTransport& out);
    static std::string transportToText(VoIP::IpcTransport transport);
    static void applyClientConfigFile(VoIP::ChannelConfig& cfg,
                                      const std::string& path);

private:
    void setupChannelEvents();
    void handleIpcMessage(const std::string& jsonMsg);
    void sendIpc(const std::string& jsonMsg);
    void sendState();
    void sendSimpleEvent(const std::string& evt);
    void sendError(const std::string& msg);

    CVoIPClientAppOptions m_options;
    VoIP::Channel m_channel;
    VoIP::ChannelConfig m_cfg;
    VoIP::ChannelEvents m_events;
    VoIP::IpcServer m_ipc;
    mutable std::mutex m_channelMtx;
    mutable std::mutex m_cfgMtx;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_joined{false};
    std::atomic<int64_t> m_lastIpcHeartbeatMs{0};
    bool m_started = false;
};
