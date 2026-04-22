#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace VoIP {

constexpr int IPC_VERSION = 1;
constexpr uint16_t IPC_DEFAULT_SOCKET_PORT = 17832;
constexpr const char* IPC_DEFAULT_PIPE_NAME = "RanOnlineVoIP";

enum class IpcTransport {
    Socket,
    NamedPipe
};

struct IpcOptions {
    IpcTransport transport = IpcTransport::Socket;
    uint16_t socketPort = IPC_DEFAULT_SOCKET_PORT;
    std::string pipeName = IPC_DEFAULT_PIPE_NAME;
};

using IpcMessageCallback = std::function<void(const std::string& jsonMsg)>;

class IpcServer {
public:
    IpcServer();
    ~IpcServer();

    bool start(const IpcOptions& options, IpcMessageCallback onMessage);
    void stop();
    void send(const std::string& jsonMsg);

private:
    struct Impl;
    Impl* m_impl = nullptr;
};

class IpcClient {
public:
    IpcClient();
    ~IpcClient();

    bool connect(const IpcOptions& options, IpcMessageCallback onEvent);
    void disconnect();
    void send(const std::string& jsonMsg);

private:
    struct Impl;
    Impl* m_impl = nullptr;
};

} // namespace VoIP
