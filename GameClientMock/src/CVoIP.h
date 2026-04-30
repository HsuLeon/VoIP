#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

struct CVoIPIpcOptions {
    enum class Transport {
        Socket,
        NamedPipe
    };

    static constexpr uint16_t DEFAULT_SOCKET_PORT = 17832;
    static constexpr const char* DEFAULT_PIPE_NAME = "RanOnlineVoIP";

    Transport transport = Transport::Socket;
    uint16_t socketPort = DEFAULT_SOCKET_PORT;
    std::string pipeName = DEFAULT_PIPE_NAME;
};

struct CVoIPOptions {
    CVoIPIpcOptions ipc;

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
    bool connectIpc();
    void disconnectIpc();
    bool isChildRunning() const;
    void terminateChildIfRunning();
    void closeChildHandles();
    void startHeartbeat();
    void stopHeartbeat();
    void sendRaw(const std::string& json);
    void startReceiveLoop();

    CVoIPOptions m_options;
    ChildProcess m_child;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_quitSent{false};
    std::thread m_heartbeatThread;
    std::thread m_recvThread;
    void* m_pipeRead = nullptr;
    void* m_pipeWrite = nullptr;
    uintptr_t m_socket = static_cast<uintptr_t>(~0ULL);
    bool m_wsaStarted = false;
    std::string m_recvRemainder;
};
