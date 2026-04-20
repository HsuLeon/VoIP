// Ipc.cpp – VoIP.exe <-> 遊戲 Client IPC 通訊實作
// 協定：JSON over TCP 127.0.0.1:{IPC_PORT}
// 訊息格式：每條訊息以 '\n' 結尾，recv 端按行切分

// Windows 標頭衝突防護（必須在 winsock2.h 之前）
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "VoIP/Ipc.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <thread>
#include <atomic>
#include <mutex>
#include <string>

#pragma comment(lib, "ws2_32.lib")

namespace VoIP {

// ── 工具函式：按行讀取 TCP 串流 ─────────────────────────────
// 從 sock 接收資料，填入 lineBuf 直到遇到 '\n'；
// 每次呼叫完成一行後回傳 true，連線斷開回傳 false。
static bool recvLine(SOCKET sock, std::string& lineBuf, std::string& remaining) {
    // 先從剩餘資料中找換行
    auto pos = remaining.find('\n');
    if (pos != std::string::npos) {
        lineBuf   = remaining.substr(0, pos);
        remaining = remaining.substr(pos + 1);
        return true;
    }

    // 持續接收直到找到換行或連線斷開
    char buf[4096];
    while (true) {
        int n = recv(sock, buf, sizeof(buf) - 1, 0);
        if (n <= 0) return false; // 連線斷開或錯誤
        buf[n] = '\0';
        remaining += buf;

        pos = remaining.find('\n');
        if (pos != std::string::npos) {
            lineBuf   = remaining.substr(0, pos);
            remaining = remaining.substr(pos + 1);
            return true;
        }
    }
}

// ═══════════════════════════════════════════════════════════════
//  IpcServer（VoIP.exe 端）
// ═══════════════════════════════════════════════════════════════

struct IpcServer::Impl {
    SOCKET             listenSock  = INVALID_SOCKET;
    SOCKET             clientSock  = INVALID_SOCKET;
    std::thread        acceptThread;
    std::thread        recvThread;
    std::atomic<bool>  running{false};
    IpcMessageCallback onMessage;
    std::mutex         sendMtx;
};

IpcServer::IpcServer()  : m_impl(new Impl) {}
IpcServer::~IpcServer() { stop(); delete m_impl; }

bool IpcServer::start(IpcMessageCallback onMessage) {
    m_impl->onMessage = onMessage;

    WSADATA wsaData{};
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
        return false;

    m_impl->listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_impl->listenSock == INVALID_SOCKET) {
        WSACleanup();
        return false;
    }

    // 允許快速重啟時重用位址
    BOOL reuse = TRUE;
    setsockopt(m_impl->listenSock, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // 127.0.0.1
    addr.sin_port        = htons(IPC_PORT);

    if (::bind(m_impl->listenSock,
               reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        closesocket(m_impl->listenSock);
        m_impl->listenSock = INVALID_SOCKET;
        WSACleanup();
        return false;
    }

    if (listen(m_impl->listenSock, 1) != 0) {
        closesocket(m_impl->listenSock);
        m_impl->listenSock = INVALID_SOCKET;
        WSACleanup();
        return false;
    }

    m_impl->running = true;

    // 接受遊戲 Client 連線（只接受一個）
    m_impl->acceptThread = std::thread([this]() {
        while (m_impl->running.load()) {
            sockaddr_in clientAddr{};
            int         clientLen = sizeof(clientAddr);
            SOCKET      newSock   = accept(m_impl->listenSock,
                                           reinterpret_cast<sockaddr*>(&clientAddr),
                                           &clientLen);
            if (newSock == INVALID_SOCKET) {
                if (!m_impl->running.load()) break;
                continue;
            }

            // 關閉舊的連線（若有）
            {
                std::lock_guard<std::mutex> lock(m_impl->sendMtx);
                if (m_impl->clientSock != INVALID_SOCKET) {
                    closesocket(m_impl->clientSock);
                }
                m_impl->clientSock = newSock;
            }

            // 啟動接收執行緒處理此 Client
            if (m_impl->recvThread.joinable())
                m_impl->recvThread.join();

            m_impl->recvThread = std::thread([this, newSock]() {
                std::string remaining;
                std::string line;
                while (m_impl->running.load()) {
                    if (!recvLine(newSock, line, remaining)) break;
                    if (!line.empty() && m_impl->onMessage)
                        m_impl->onMessage(line);
                }
            });
        }
    });

    return true;
}

void IpcServer::stop() {
    m_impl->running = false;

    if (m_impl->listenSock != INVALID_SOCKET) {
        closesocket(m_impl->listenSock);
        m_impl->listenSock = INVALID_SOCKET;
    }
    {
        std::lock_guard<std::mutex> lock(m_impl->sendMtx);
        if (m_impl->clientSock != INVALID_SOCKET) {
            closesocket(m_impl->clientSock);
            m_impl->clientSock = INVALID_SOCKET;
        }
    }

    if (m_impl->acceptThread.joinable()) m_impl->acceptThread.join();
    if (m_impl->recvThread.joinable())   m_impl->recvThread.join();

    WSACleanup();
}

void IpcServer::send(const std::string& jsonMsg) {
    std::lock_guard<std::mutex> lock(m_impl->sendMtx);
    if (m_impl->clientSock == INVALID_SOCKET) return;

    // 訊息以換行結尾
    std::string msg = jsonMsg;
    if (msg.empty() || msg.back() != '\n')
        msg += '\n';

    int total = static_cast<int>(msg.size());
    int sent  = 0;
    while (sent < total) {
        int n = ::send(m_impl->clientSock,
                       msg.c_str() + sent, total - sent, 0);
        if (n <= 0) return; // 連線斷開
        sent += n;
    }
}

// ═══════════════════════════════════════════════════════════════
//  IpcClient（遊戲 Client 端）
// ═══════════════════════════════════════════════════════════════

struct IpcClient::Impl {
    SOCKET             sock = INVALID_SOCKET;
    std::thread        recvThread;
    std::atomic<bool>  running{false};
    IpcMessageCallback onEvent;
    std::mutex         sendMtx;
};

IpcClient::IpcClient()  : m_impl(new Impl) {}
IpcClient::~IpcClient() { disconnect(); delete m_impl; }

bool IpcClient::connect(IpcMessageCallback onEvent) {
    m_impl->onEvent = onEvent;

    WSADATA wsaData{};
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
        return false;

    m_impl->sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_impl->sock == INVALID_SOCKET) {
        WSACleanup();
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // 127.0.0.1
    addr.sin_port        = htons(IPC_PORT);

    if (::connect(m_impl->sock,
                  reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        closesocket(m_impl->sock);
        m_impl->sock = INVALID_SOCKET;
        WSACleanup();
        return false;
    }

    m_impl->running = true;

    // 背景執行緒持續接收並回呼
    m_impl->recvThread = std::thread([this]() {
        std::string remaining;
        std::string line;
        while (m_impl->running.load()) {
            if (!recvLine(m_impl->sock, line, remaining)) break;
            if (!line.empty() && m_impl->onEvent)
                m_impl->onEvent(line);
        }
    });

    return true;
}

void IpcClient::disconnect() {
    m_impl->running = false;
    {
        std::lock_guard<std::mutex> lock(m_impl->sendMtx);
        if (m_impl->sock != INVALID_SOCKET) {
            closesocket(m_impl->sock);
            m_impl->sock = INVALID_SOCKET;
        }
    }
    if (m_impl->recvThread.joinable())
        m_impl->recvThread.join();
    WSACleanup();
}

void IpcClient::send(const std::string& jsonMsg) {
    std::lock_guard<std::mutex> lock(m_impl->sendMtx);
    if (m_impl->sock == INVALID_SOCKET) return;

    std::string msg = jsonMsg;
    if (msg.empty() || msg.back() != '\n')
        msg += '\n';

    int total = static_cast<int>(msg.size());
    int sent  = 0;
    while (sent < total) {
        int n = ::send(m_impl->sock,
                       msg.c_str() + sent, total - sent, 0);
        if (n <= 0) return;
        sent += n;
    }
}

} // namespace VoIP
