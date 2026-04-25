#pragma once

#include "VoIP/VoIP.h"

#include <atomic>
#include <string>
#include <thread>

struct CVoIPOptions {
    VoIP::IpcOptions ipc;

#ifdef _DEBUG
    bool showVoipConsole = true;
#else
    bool showVoipConsole = false;
#endif
};

class CVoIP {
public:
    static CVoIP& instance();

    CVoIP(const CVoIP&) = delete;
    CVoIP& operator=(const CVoIP&) = delete;

    bool start(const CVoIPOptions& options);
    void stop();

    void hello();
    void status();
    void login(const std::string& playerId,
               const std::string& token,
               const std::string& server,
               const std::string& channelId);
    void join(const std::string& channelId);
    void leave();
    void setMute(bool muted);
    void toggleMute();
    void quitClient();

private:
    CVoIP();
    ~CVoIP();

    struct ChildProcess {
        void* process = nullptr;
        void* thread = nullptr;
        bool launched = false;
    };

    bool tryConnectExistingIpc();
    bool launchVoipClient();
    bool connectLaunchedIpc();
    bool isChildRunning() const;
    void terminateChildIfRunning();
    void closeChildHandles();
    void startHeartbeat();
    void stopHeartbeat();
    void sendRaw(const std::string& json);

    CVoIPOptions m_options;
    VoIP::IpcClient m_ipc;
    ChildProcess m_child;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_quitSent{false};
    std::thread m_heartbeatThread;
};
