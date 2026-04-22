#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "VoIP/Ipc.h"

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

#pragma comment(lib, "ws2_32.lib")

namespace VoIP {
namespace {

std::string ensureLineMessage(const std::string& jsonMsg) {
    if (!jsonMsg.empty() && jsonMsg.back() == '\n') {
        return jsonMsg;
    }
    return jsonMsg + "\n";
}

bool recvSocketLine(SOCKET sock, std::string& lineBuf, std::string& remaining) {
    auto pos = remaining.find('\n');
    if (pos != std::string::npos) {
        lineBuf = remaining.substr(0, pos);
        remaining = remaining.substr(pos + 1);
        return true;
    }

    char buf[4096];
    while (true) {
        const int n = recv(sock, buf, sizeof(buf) - 1, 0);
        if (n <= 0) return false;
        buf[n] = '\0';
        remaining += buf;

        pos = remaining.find('\n');
        if (pos != std::string::npos) {
            lineBuf = remaining.substr(0, pos);
            remaining = remaining.substr(pos + 1);
            return true;
        }
    }
}

bool sendSocketAll(SOCKET sock, const std::string& jsonMsg) {
    const std::string msg = ensureLineMessage(jsonMsg);
    const int total = static_cast<int>(msg.size());
    int sent = 0;
    while (sent < total) {
        const int n = ::send(sock, msg.c_str() + sent, total - sent, 0);
        if (n <= 0) return false;
        sent += n;
    }
    return true;
}

std::string makePipePath(const std::string& pipeName) {
    return "\\\\.\\pipe\\" + pipeName;
}

std::string makePipeRxPath(const std::string& pipeName) {
    return makePipePath(pipeName) + ".c2s";
}

std::string makePipeTxPath(const std::string& pipeName) {
    return makePipePath(pipeName) + ".s2c";
}

bool recvPipeLine(HANDLE pipe, std::string& lineBuf, std::string& remaining) {
    auto pos = remaining.find('\n');
    if (pos != std::string::npos) {
        lineBuf = remaining.substr(0, pos);
        remaining = remaining.substr(pos + 1);
        return true;
    }

    char buf[4096];
    while (true) {
        DWORD bytesRead = 0;
        const BOOL ok = ReadFile(pipe, buf, sizeof(buf), &bytesRead, nullptr);
        if (!ok || bytesRead == 0) return false;
        remaining.append(buf, buf + bytesRead);

        pos = remaining.find('\n');
        if (pos != std::string::npos) {
            lineBuf = remaining.substr(0, pos);
            remaining = remaining.substr(pos + 1);
            return true;
        }
    }
}

bool sendPipeAll(HANDLE pipe, const std::string& jsonMsg) {
    const std::string msg = ensureLineMessage(jsonMsg);
    const char* data = msg.data();
    DWORD totalWritten = 0;
    while (totalWritten < msg.size()) {
        DWORD written = 0;
        const BOOL ok = WriteFile(pipe,
                                  data + totalWritten,
                                  static_cast<DWORD>(msg.size() - totalWritten),
                                  &written,
                                  nullptr);
        if (!ok) return false;
        totalWritten += written;
    }
    return true;
}

bool openClientPipeWithRetry(const std::string& path, DWORD desiredAccess, HANDLE& outHandle) {
    for (int i = 0; i < 30; ++i) {
        if (WaitNamedPipeA(path.c_str(), 200)) {
            outHandle = CreateFileA(path.c_str(),
                                    desiredAccess,
                                    0,
                                    nullptr,
                                    OPEN_EXISTING,
                                    0,
                                    nullptr);
            if (outHandle != INVALID_HANDLE_VALUE) {
                return true;
            }
        }
    }
    return false;
}

} // namespace

struct IpcServer::Impl {
    IpcOptions options;
    IpcMessageCallback onMessage;
    std::atomic<bool> running{false};
    std::mutex sendMtx;

    SOCKET listenSock = INVALID_SOCKET;
    SOCKET clientSock = INVALID_SOCKET;

    HANDLE pipeRead = INVALID_HANDLE_VALUE;
    HANDLE pipeWrite = INVALID_HANDLE_VALUE;

    std::thread acceptThread;
    std::thread recvThread;
    bool wsaStarted = false;
};

IpcServer::IpcServer() : m_impl(new Impl) {}

IpcServer::~IpcServer() {
    stop();
    delete m_impl;
}

bool IpcServer::start(const IpcOptions& options, IpcMessageCallback onMessage) {
    stop();

    m_impl->options = options;
    m_impl->onMessage = onMessage;
    m_impl->running = true;

    if (options.transport == IpcTransport::Socket) {
        WSADATA wsaData{};
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            m_impl->running = false;
            return false;
        }
        m_impl->wsaStarted = true;

        m_impl->listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (m_impl->listenSock == INVALID_SOCKET) {
            stop();
            return false;
        }

        BOOL reuse = TRUE;
        setsockopt(m_impl->listenSock,
                   SOL_SOCKET,
                   SO_REUSEADDR,
                   reinterpret_cast<const char*>(&reuse),
                   sizeof(reuse));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(options.socketPort);

        if (::bind(m_impl->listenSock,
                   reinterpret_cast<sockaddr*>(&addr),
                   sizeof(addr)) != 0) {
            stop();
            return false;
        }

        if (listen(m_impl->listenSock, 1) != 0) {
            stop();
            return false;
        }

        m_impl->acceptThread = std::thread([this]() {
            while (m_impl->running.load()) {
                sockaddr_in clientAddr{};
                int clientLen = sizeof(clientAddr);
                const SOCKET newSock =
                    accept(m_impl->listenSock,
                           reinterpret_cast<sockaddr*>(&clientAddr),
                           &clientLen);
                if (newSock == INVALID_SOCKET) {
                    if (!m_impl->running.load()) break;
                    continue;
                }

                {
                    std::lock_guard<std::mutex> lock(m_impl->sendMtx);
                    if (m_impl->clientSock != INVALID_SOCKET) {
                        closesocket(m_impl->clientSock);
                    }
                    m_impl->clientSock = newSock;
                }

                if (m_impl->recvThread.joinable()) {
                    m_impl->recvThread.join();
                }

                m_impl->recvThread = std::thread([this, newSock]() {
                    std::string remaining;
                    std::string line;
                    while (m_impl->running.load()) {
                        if (!recvSocketLine(newSock, line, remaining)) break;
                        if (!line.empty() && m_impl->onMessage) {
                            m_impl->onMessage(line);
                        }
                    }
                });
            }
        });

        return true;
    }

    m_impl->acceptThread = std::thread([this]() {
        const std::string rxPath = makePipeRxPath(m_impl->options.pipeName);
        const std::string txPath = makePipeTxPath(m_impl->options.pipeName);

        while (m_impl->running.load()) {
            HANDLE readPipe = CreateNamedPipeA(
                rxPath.c_str(),
                PIPE_ACCESS_INBOUND,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                1,
                4096,
                4096,
                0,
                nullptr);
            HANDLE writePipe = CreateNamedPipeA(
                txPath.c_str(),
                PIPE_ACCESS_OUTBOUND,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                1,
                4096,
                4096,
                0,
                nullptr);

            if (readPipe == INVALID_HANDLE_VALUE || writePipe == INVALID_HANDLE_VALUE) {
                if (readPipe != INVALID_HANDLE_VALUE) CloseHandle(readPipe);
                if (writePipe != INVALID_HANDLE_VALUE) CloseHandle(writePipe);
                break;
            }

            const BOOL readConnected =
                ConnectNamedPipe(readPipe, nullptr)
                    ? TRUE
                    : (GetLastError() == ERROR_PIPE_CONNECTED ? TRUE : FALSE);
            const BOOL writeConnected =
                ConnectNamedPipe(writePipe, nullptr)
                    ? TRUE
                    : (GetLastError() == ERROR_PIPE_CONNECTED ? TRUE : FALSE);

            if (!readConnected || !writeConnected || !m_impl->running.load()) {
                DisconnectNamedPipe(readPipe);
                DisconnectNamedPipe(writePipe);
                CloseHandle(readPipe);
                CloseHandle(writePipe);
                if (!m_impl->running.load()) break;
                continue;
            }

            {
                std::lock_guard<std::mutex> lock(m_impl->sendMtx);
                m_impl->pipeRead = readPipe;
                m_impl->pipeWrite = writePipe;
            }

            if (m_impl->recvThread.joinable()) {
                m_impl->recvThread.join();
            }

            m_impl->recvThread = std::thread([this, readPipe, writePipe]() {
                std::string remaining;
                std::string line;
                while (m_impl->running.load()) {
                    if (!recvPipeLine(readPipe, line, remaining)) break;
                    if (!line.empty() && m_impl->onMessage) {
                        m_impl->onMessage(line);
                    }
                }

                std::lock_guard<std::mutex> lock(m_impl->sendMtx);
                if (m_impl->pipeRead == readPipe) {
                    FlushFileBuffers(writePipe);
                    DisconnectNamedPipe(readPipe);
                    DisconnectNamedPipe(writePipe);
                    CloseHandle(readPipe);
                    CloseHandle(writePipe);
                    m_impl->pipeRead = INVALID_HANDLE_VALUE;
                    m_impl->pipeWrite = INVALID_HANDLE_VALUE;
                }
            });

            if (m_impl->recvThread.joinable()) {
                m_impl->recvThread.join();
            }
        }
    });

    return true;
}

void IpcServer::stop() {
    m_impl->running = false;

    if (m_impl->acceptThread.joinable()) {
        CancelSynchronousIo(m_impl->acceptThread.native_handle());
    }
    if (m_impl->recvThread.joinable()) {
        CancelSynchronousIo(m_impl->recvThread.native_handle());
    }

    {
        std::lock_guard<std::mutex> lock(m_impl->sendMtx);
        if (m_impl->clientSock != INVALID_SOCKET) {
            closesocket(m_impl->clientSock);
            m_impl->clientSock = INVALID_SOCKET;
        }
        if (m_impl->listenSock != INVALID_SOCKET) {
            closesocket(m_impl->listenSock);
            m_impl->listenSock = INVALID_SOCKET;
        }
        if (m_impl->pipeRead != INVALID_HANDLE_VALUE) {
            DisconnectNamedPipe(m_impl->pipeRead);
            CloseHandle(m_impl->pipeRead);
            m_impl->pipeRead = INVALID_HANDLE_VALUE;
        }
        if (m_impl->pipeWrite != INVALID_HANDLE_VALUE) {
            FlushFileBuffers(m_impl->pipeWrite);
            DisconnectNamedPipe(m_impl->pipeWrite);
            CloseHandle(m_impl->pipeWrite);
            m_impl->pipeWrite = INVALID_HANDLE_VALUE;
        }
    }

    if (m_impl->acceptThread.joinable()) {
        m_impl->acceptThread.join();
    }
    if (m_impl->recvThread.joinable()) {
        m_impl->recvThread.join();
    }

    if (m_impl->wsaStarted) {
        WSACleanup();
        m_impl->wsaStarted = false;
    }
}

void IpcServer::send(const std::string& jsonMsg) {
    std::lock_guard<std::mutex> lock(m_impl->sendMtx);

    if (m_impl->options.transport == IpcTransport::Socket) {
        if (m_impl->clientSock == INVALID_SOCKET) return;
        sendSocketAll(m_impl->clientSock, jsonMsg);
        return;
    }

    if (m_impl->pipeWrite == INVALID_HANDLE_VALUE) return;
    sendPipeAll(m_impl->pipeWrite, jsonMsg);
}

struct IpcClient::Impl {
    IpcOptions options;
    IpcMessageCallback onEvent;
    std::atomic<bool> running{false};
    std::mutex sendMtx;

    SOCKET sock = INVALID_SOCKET;
    HANDLE pipeRead = INVALID_HANDLE_VALUE;
    HANDLE pipeWrite = INVALID_HANDLE_VALUE;
    std::thread recvThread;
    bool wsaStarted = false;
};

IpcClient::IpcClient() : m_impl(new Impl) {}

IpcClient::~IpcClient() {
    disconnect();
    delete m_impl;
}

bool IpcClient::connect(const IpcOptions& options, IpcMessageCallback onEvent) {
    disconnect();

    m_impl->options = options;
    m_impl->onEvent = onEvent;

    if (options.transport == IpcTransport::Socket) {
        WSADATA wsaData{};
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            return false;
        }
        m_impl->wsaStarted = true;

        m_impl->sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (m_impl->sock == INVALID_SOCKET) {
            disconnect();
            return false;
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(options.socketPort);

        if (::connect(m_impl->sock,
                      reinterpret_cast<sockaddr*>(&addr),
                      sizeof(addr)) != 0) {
            disconnect();
            return false;
        }

        m_impl->running = true;
        m_impl->recvThread = std::thread([this]() {
            std::string remaining;
            std::string line;
            while (m_impl->running.load()) {
                if (!recvSocketLine(m_impl->sock, line, remaining)) break;
                if (!line.empty() && m_impl->onEvent) {
                    m_impl->onEvent(line);
                }
            }
        });
        return true;
    }

    const std::string rxPath = makePipeTxPath(options.pipeName);
    const std::string txPath = makePipeRxPath(options.pipeName);
    if (!openClientPipeWithRetry(rxPath, GENERIC_READ, m_impl->pipeRead) ||
        !openClientPipeWithRetry(txPath, GENERIC_WRITE, m_impl->pipeWrite)) {
        disconnect();
        return false;
    }

    m_impl->running = true;
    m_impl->recvThread = std::thread([this]() {
        std::string remaining;
        std::string line;
        while (m_impl->running.load()) {
            if (!recvPipeLine(m_impl->pipeRead, line, remaining)) break;
            if (!line.empty() && m_impl->onEvent) {
                m_impl->onEvent(line);
            }
        }
    });
    return true;
}

void IpcClient::disconnect() {
    m_impl->running = false;

    if (m_impl->recvThread.joinable()) {
        CancelSynchronousIo(m_impl->recvThread.native_handle());
    }

    {
        std::lock_guard<std::mutex> lock(m_impl->sendMtx);
        if (m_impl->sock != INVALID_SOCKET) {
            closesocket(m_impl->sock);
            m_impl->sock = INVALID_SOCKET;
        }
        if (m_impl->pipeRead != INVALID_HANDLE_VALUE) {
            CloseHandle(m_impl->pipeRead);
            m_impl->pipeRead = INVALID_HANDLE_VALUE;
        }
        if (m_impl->pipeWrite != INVALID_HANDLE_VALUE) {
            CloseHandle(m_impl->pipeWrite);
            m_impl->pipeWrite = INVALID_HANDLE_VALUE;
        }
    }

    if (m_impl->recvThread.joinable()) {
        m_impl->recvThread.join();
    }

    if (m_impl->wsaStarted) {
        WSACleanup();
        m_impl->wsaStarted = false;
    }
}

void IpcClient::send(const std::string& jsonMsg) {
    std::lock_guard<std::mutex> lock(m_impl->sendMtx);

    if (m_impl->options.transport == IpcTransport::Socket) {
        if (m_impl->sock == INVALID_SOCKET) return;
        sendSocketAll(m_impl->sock, jsonMsg);
        return;
    }

    if (m_impl->pipeWrite == INVALID_HANDLE_VALUE) return;
    sendPipeAll(m_impl->pipeWrite, jsonMsg);
}

} // namespace VoIP
