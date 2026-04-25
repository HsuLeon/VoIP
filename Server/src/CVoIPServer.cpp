#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

#include "CVoIPServer.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>

CVoIPServer& CVoIPServer::instance() {
    static CVoIPServer instance;
    return instance;
}

CVoIPServer::CVoIPServer() = default;

CVoIPServer::~CVoIPServer() = default;

uint16_t CVoIPServer::ClientSession::relayTargetPort() const {
    return udpAddrKnown ? udpExtPort : publicPort;
}

const std::string& CVoIPServer::ClientSession::relayTargetIp() const {
    return udpAddrKnown ? udpExtIp : publicIp;
}

bool CVoIPServer::ClientSession::sendLine(const std::string& msg) {
    std::lock_guard<std::mutex> lk(sendMtx);
    std::string line = msg + '\n';
    const char* ptr = line.c_str();
    int total = static_cast<int>(line.size());
    int sent = 0;
    while (sent < total) {
        const int n = ::send(sock, ptr + sent, total - sent, 0);
        if (n <= 0) {
            alive = false;
            return false;
        }
        sent += n;
    }
    return true;
}

void CVoIPServer::requestStop() {
    if (!m_running.exchange(false)) return;
    std::cout << "\n[Server] Shutting down...\n";
}

std::string CVoIPServer::jStr(const std::string& j, const std::string& key) {
    const std::string pat = "\"" + key + "\"";
    const auto k = j.find(pat);
    if (k == std::string::npos) return {};
    const auto c = j.find(':', k + pat.size());
    if (c == std::string::npos) return {};
    const auto q1 = j.find('"', c + 1);
    if (q1 == std::string::npos) return {};
    const auto q2 = j.find('"', q1 + 1);
    if (q2 == std::string::npos) return {};
    return j.substr(q1 + 1, q2 - q1 - 1);
}

int CVoIPServer::jInt(const std::string& j,
                      const std::string& key,
                      int def) {
    const std::string pat = "\"" + key + "\"";
    const auto k = j.find(pat);
    if (k == std::string::npos) return def;
    const auto c = j.find(':', k + pat.size());
    if (c == std::string::npos) return def;
    size_t v = c + 1;
    while (v < j.size() && (j[v] == ' ' || j[v] == '\t' || j[v] == '\r')) ++v;
    if (v >= j.size()) return def;
    char* end = nullptr;
    const long val = strtol(j.c_str() + v, &end, 10);
    return (end > j.c_str() + v) ? static_cast<int>(val) : def;
}

bool CVoIPServer::isJsonRawValue(const std::string& v) {
    if (v.empty()) return false;
    if (v == "true" || v == "false" || v == "null") return true;
    if (v[0] == '[' || v[0] == '{') return true;

    size_t i = 0;
    if (v[i] == '-') ++i;
    bool hasDigits = false;
    bool hasDot = false;
    for (; i < v.size(); ++i) {
        const unsigned char ch = static_cast<unsigned char>(v[i]);
        if (std::isdigit(ch)) {
            hasDigits = true;
            continue;
        }
        if (v[i] == '.' && !hasDot) {
            hasDot = true;
            continue;
        }
        return false;
    }
    return hasDigits;
}

std::string CVoIPServer::jBuild(
    const std::vector<std::pair<std::string, std::string>>& fields) {
    std::string s = "{";
    for (size_t i = 0; i < fields.size(); ++i) {
        if (i) s += ',';
        s += '"' + fields[i].first + "\":";
        const std::string& v = fields[i].second;
        if (isJsonRawValue(v)) {
            s += v;
        } else {
            s += '"' + v + '"';
        }
    }
    return s + '}';
}

bool CVoIPServer::parseRtpSsrc(const uint8_t* data,
                               int len,
                               uint32_t& ssrcOut) {
    if (len < 12) return false;
    if ((data[0] & 0xC0) != 0x80) return false;
    ssrcOut = (uint32_t(data[8]) << 24) | (uint32_t(data[9]) << 16) |
              (uint32_t(data[10]) << 8) | uint32_t(data[11]);
    return true;
}

std::vector<std::shared_ptr<CVoIPServer::ClientSession>>
CVoIPServer::getChannelPeers(const std::string& channelId,
                             const std::string& excludePlayerId) {
    std::vector<std::shared_ptr<ClientSession>> result;
    std::lock_guard<std::mutex> lk(m_sessionsMtx);
    for (auto& [pid, s] : m_sessions) {
        if (s->channelId == channelId && pid != excludePlayerId && s->alive) {
            result.push_back(s);
        }
    }
    return result;
}

void CVoIPServer::broadcastToChannel(const std::string& channelId,
                                     const std::string& excludePlayerId,
                                     const std::string& msg) {
    for (auto& s : getChannelPeers(channelId, excludePlayerId)) {
        s->sendLine(msg);
    }
}

void CVoIPServer::broadcastPeerAddr(const std::shared_ptr<ClientSession>& session,
                                    const std::string& excludePlayerId) {
    const std::string peerAddrMsg = jBuild({
        {"cmd", "PEER_ADDR"},
        {"playerId", session->playerId},
        {"lanIp", session->lanIp},
        {"lanPort", std::to_string(session->localUdpPort)},
        {"publicIp", session->publicIp},
        {"publicPort", std::to_string(session->publicPort)}
    });
    broadcastToChannel(session->channelId, excludePlayerId, peerAddrMsg);
}

void CVoIPServer::handleClient(SOCKET clientSock, std::string peerIp) {
    DWORD tv = 10000;
    setsockopt(clientSock, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&tv), sizeof(tv));

    std::string recvBuf;
    std::string joinLine;
    {
        char buf[4096];
        while (true) {
            const int n = recv(clientSock, buf, static_cast<int>(sizeof(buf)) - 1, 0);
            if (n <= 0) {
                closesocket(clientSock);
                return;
            }
            buf[n] = '\0';
            recvBuf += buf;
            const auto pos = recvBuf.find('\n');
            if (pos != std::string::npos) {
                joinLine = recvBuf.substr(0, pos);
                recvBuf = recvBuf.substr(pos + 1);
                break;
            }
        }
    }

    if (jStr(joinLine, "cmd") != "JOIN") {
        std::cout << "[SIG] " << peerIp << " sent unexpected cmd, closing\n";
        closesocket(clientSock);
        return;
    }

    auto session = std::make_shared<ClientSession>();
    session->sock = clientSock;
    session->playerId = jStr(joinLine, "playerId");
    session->channelId = jStr(joinLine, "channelId");
    session->localUdpPort = static_cast<uint16_t>(jInt(joinLine, "udpPort"));
    session->lanIp = jStr(joinLine, "lanIp");
    session->publicIp = jStr(joinLine, "publicIp");
    session->publicPort = static_cast<uint16_t>(jInt(joinLine, "publicPort"));
    if (session->publicIp.empty()) session->publicIp = peerIp;
    if (session->publicPort == 0) session->publicPort = session->localUdpPort;
    session->ssrc = static_cast<uint32_t>(jInt(joinLine, "ssrc"));
    session->lastPong = std::chrono::steady_clock::now();

    if (session->playerId.empty() || session->channelId.empty()) {
        std::cout << "[SIG] " << peerIp << " JOIN missing fields, closing\n";
        closesocket(clientSock);
        return;
    }

    std::cout << "[SIG] JOIN  player=" << session->playerId
              << "  channel=" << session->channelId
              << "  tcpIp=" << peerIp
              << "  localUdpPort=" << session->localUdpPort
              << "  lanIp=" << (session->lanIp.empty() ? "-" : session->lanIp)
              << "  advertised=" << session->publicIp << ":" << session->publicPort
              << "  ssrc=" << session->ssrc << "\n";

    {
        std::string peersArr = "[";
        {
            std::lock_guard<std::mutex> lk(m_sessionsMtx);

            const auto it = m_sessions.find(session->playerId);
            if (it != m_sessions.end()) {
                it->second->alive = false;
                closesocket(it->second->sock);
                m_sessions.erase(it);
            }

            bool first = true;
            for (auto& [pid, s] : m_sessions) {
                if (s->channelId == session->channelId && s->alive) {
                    if (!first) peersArr += ',';
                    peersArr += jBuild({
                        {"playerId", s->playerId},
                        {"lanIp", s->lanIp},
                        {"lanPort", std::to_string(s->localUdpPort)},
                        {"publicIp", s->publicIp},
                        {"publicPort", std::to_string(s->publicPort)},
                        {"ssrc", std::to_string(s->ssrc)}
                    });
                    first = false;
                }
            }

            m_sessions[session->playerId] = session;
        }
        peersArr += ']';

        const std::string joinedMsg = jBuild({
            {"cmd", "JOINED"},
            {"relayPort", std::to_string(RELAY_PORT)},
            {"peers", peersArr}
        });

        DWORD noTv = 0;
        setsockopt(clientSock, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&noTv), sizeof(noTv));

        if (!session->sendLine(joinedMsg)) {
            std::lock_guard<std::mutex> lk(m_sessionsMtx);
            m_sessions.erase(session->playerId);
            closesocket(clientSock);
            return;
        }
    }

    {
        const std::string peerJoinMsg = jBuild({
            {"cmd", "PEER_JOIN"},
            {"playerId", session->playerId},
            {"lanIp", session->lanIp},
            {"lanPort", std::to_string(session->localUdpPort)},
            {"publicIp", session->publicIp},
            {"publicPort", std::to_string(session->publicPort)},
            {"ssrc", std::to_string(session->ssrc)}
        });
        broadcastToChannel(session->channelId, session->playerId, peerJoinMsg);
    }

    std::cout << "[SIG] JOINED player=" << session->playerId << "\n";

    while (m_running && session->alive) {
        char buf[4096];
        const int n = recv(clientSock, buf, static_cast<int>(sizeof(buf)) - 1, 0);
        if (n <= 0) break;

        buf[n] = '\0';
        recvBuf += buf;

        size_t pos = 0;
        while ((pos = recvBuf.find('\n')) != std::string::npos) {
            std::string line = recvBuf.substr(0, pos);
            recvBuf = recvBuf.substr(pos + 1);
            if (line.empty()) continue;

            const std::string cmd = jStr(line, "cmd");

            if (cmd == "LEAVE") {
                session->alive = false;
                break;
            } else if (cmd == "PONG") {
                session->lastPong = std::chrono::steady_clock::now();
            } else if (cmd == "UPDATE_ADDR") {
                const uint16_t udpPort = static_cast<uint16_t>(jInt(line, "udpPort"));
                const std::string lanIp = jStr(line, "lanIp");
                const std::string publicIp = jStr(line, "publicIp");
                const uint16_t publicPort =
                    static_cast<uint16_t>(jInt(line, "publicPort"));

                if (udpPort != 0) session->localUdpPort = udpPort;
                if (!lanIp.empty()) session->lanIp = lanIp;
                if (!publicIp.empty()) session->publicIp = publicIp;
                if (publicPort != 0) session->publicPort = publicPort;

                std::cout << "[SIG] UPDATE_ADDR player=" << session->playerId
                          << " lanIp="
                          << (session->lanIp.empty() ? "-" : session->lanIp)
                          << " localUdpPort=" << session->localUdpPort
                          << " public=" << session->publicIp << ":"
                          << session->publicPort << "\n";

                broadcastPeerAddr(session, session->playerId);
            }
        }
    }

    {
        std::lock_guard<std::mutex> lk(m_sessionsMtx);
        const auto it = m_sessions.find(session->playerId);
        if (it != m_sessions.end() && it->second.get() == session.get()) {
            m_sessions.erase(it);
        }
    }

    const std::string leaveMsg = jBuild({
        {"cmd", "PEER_LEAVE"},
        {"playerId", session->playerId}
    });
    broadcastToChannel(session->channelId, session->playerId, leaveMsg);

    closesocket(clientSock);
    std::cout << "[SIG] LEAVE player=" << session->playerId << "\n";
}

void CVoIPServer::runSignalingServer() {
    SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSock == INVALID_SOCKET) {
        std::cerr << "[SIG] socket() failed: " << WSAGetLastError() << "\n";
        requestStop();
        return;
    }

    int yes = 1;
    setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&yes), sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(SIGNALING_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (::bind(listenSock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::cerr << "[SIG] bind() failed: " << WSAGetLastError() << "\n";
        closesocket(listenSock);
        requestStop();
        return;
    }
    if (listen(listenSock, SOMAXCONN) != 0) {
        std::cerr << "[SIG] listen() failed: " << WSAGetLastError() << "\n";
        closesocket(listenSock);
        requestStop();
        return;
    }

    std::cout << "[SIG] Listening on TCP:" << SIGNALING_PORT << "\n";

    while (m_running) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(listenSock, &fds);
        timeval timeout{1, 0};
        const int r = select(0, &fds, nullptr, nullptr, &timeout);
        if (r <= 0) continue;

        sockaddr_in clientAddr{};
        int addrLen = sizeof(clientAddr);
        const SOCKET clientSock =
            accept(listenSock, reinterpret_cast<sockaddr*>(&clientAddr), &addrLen);
        if (clientSock == INVALID_SOCKET) continue;

        char ipStr[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &clientAddr.sin_addr, ipStr, sizeof(ipStr));
        const std::string peerIp = ipStr;

        std::cout << "[SIG] Accept from " << peerIp << "\n";
        std::thread([this, clientSock, peerIp]() {
            handleClient(clientSock, peerIp);
        }).detach();
    }

    closesocket(listenSock);
}

void CVoIPServer::runTurnRelay() {
    SOCKET udpSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udpSock == INVALID_SOCKET) {
        std::cerr << "[TURN] socket() failed: " << WSAGetLastError() << "\n";
        requestStop();
        return;
    }

    int yes = 1;
    setsockopt(udpSock, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&yes), sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(RELAY_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (::bind(udpSock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::cerr << "[TURN] bind() failed: " << WSAGetLastError() << "\n";
        closesocket(udpSock);
        requestStop();
        return;
    }

    std::cout << "[TURN] Listening on UDP:" << RELAY_PORT << "\n";

    DWORD tv = 1000;
    setsockopt(udpSock, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&tv), sizeof(tv));

    static constexpr int BUF_SIZE = 2048;
    std::vector<uint8_t> buf(BUF_SIZE);

    while (m_running) {
        sockaddr_in fromAddr{};
        int fromLen = sizeof(fromAddr);

        const int n = recvfrom(udpSock,
                               reinterpret_cast<char*>(buf.data()),
                               BUF_SIZE,
                               0,
                               reinterpret_cast<sockaddr*>(&fromAddr),
                               &fromLen);
        if (n <= 0) continue;

        uint32_t ssrc = 0;
        if (!parseRtpSsrc(buf.data(), n, ssrc)) continue;

        char fromIpStr[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &fromAddr.sin_addr, fromIpStr, sizeof(fromIpStr));
        const uint16_t fromPort = ntohs(fromAddr.sin_port);

        std::string channelId;
        std::string senderId;
        std::shared_ptr<ClientSession> endpointUpdated;
        {
            std::lock_guard<std::mutex> lk(m_sessionsMtx);
            for (auto& [pid, s] : m_sessions) {
                if (s->ssrc == ssrc && s->alive) {
                    channelId = s->channelId;
                    senderId = pid;

                    if (!s->udpAddrKnown || s->publicIp != fromIpStr ||
                        s->publicPort != fromPort) {
                        std::lock_guard<std::mutex> ulk(s->udpAddrMtx);
                        s->udpExtIp = fromIpStr;
                        s->udpExtPort = fromPort;
                        s->udpAddrKnown = true;
                        s->publicIp = fromIpStr;
                        s->publicPort = fromPort;
                        endpointUpdated = s;
                    }
                    break;
                }
            }
        }
        if (channelId.empty()) continue;

        if (endpointUpdated) {
            broadcastPeerAddr(endpointUpdated, endpointUpdated->playerId);
        }

        auto peers = getChannelPeers(channelId, senderId);
        for (auto& peer : peers) {
            if (!peer->alive) continue;

            std::string destIp;
            uint16_t destPort = 0;
            {
                std::lock_guard<std::mutex> ulk(peer->udpAddrMtx);
                destIp = peer->relayTargetIp();
                destPort = peer->relayTargetPort();
            }
            if (destIp.empty() || destPort == 0) continue;

            sockaddr_in dest{};
            dest.sin_family = AF_INET;
            dest.sin_port = htons(destPort);
            inet_pton(AF_INET, destIp.c_str(), &dest.sin_addr);

            sendto(udpSock,
                   reinterpret_cast<const char*>(buf.data()),
                   n,
                   0,
                   reinterpret_cast<sockaddr*>(&dest),
                   sizeof(dest));
        }
    }

    closesocket(udpSock);
}

void CVoIPServer::runHeartbeat() {
    while (m_running) {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        const auto now = std::chrono::steady_clock::now();
        std::vector<std::shared_ptr<ClientSession>> all;
        {
            std::lock_guard<std::mutex> lk(m_sessionsMtx);
            for (auto& [pid, s] : m_sessions) {
                if (s->alive) all.push_back(s);
            }
        }

        for (auto& s : all) {
            const auto msSinceLastPong =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - s->lastPong)
                    .count();

            if (msSinceLastPong >= PONG_TIMEOUT_MS) {
                std::cout << "[HB] Timeout player=" << s->playerId << "\n";
                s->alive = false;
                closesocket(s->sock);
            } else if (msSinceLastPong >= PING_INTERVAL_MS) {
                s->sendLine("{\"cmd\":\"PING\"}");
            }
        }
    }
}

void CVoIPServer::cleanupSessions() {
    std::lock_guard<std::mutex> lk(m_sessionsMtx);
    for (auto& [pid, s] : m_sessions) {
        s->alive = false;
        closesocket(s->sock);
    }
    m_sessions.clear();
}

BOOL WINAPI CVoIPServer::consoleCtrlHandler(DWORD) {
    instance().requestStop();
    return TRUE;
}

int CVoIPServer::run() {
    m_running = true;
    SetConsoleCtrlHandler(consoleCtrlHandler, TRUE);

    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::cerr << "[Server] WSAStartup failed\n";
        return 1;
    }

    std::cout << "========================================\n";
    std::cout << "  VoIP Server\n";
    std::cout << "  Signaling : TCP:" << SIGNALING_PORT << "\n";
    std::cout << "  TURN Relay: UDP:" << RELAY_PORT << "\n";
    std::cout << "========================================\n";

    std::thread sigThread([this]() { runSignalingServer(); });
    std::thread turnThread([this]() { runTurnRelay(); });
    std::thread hbThread([this]() { runHeartbeat(); });

    sigThread.join();
    turnThread.join();
    hbThread.join();

    cleanupSessions();

    WSACleanup();
    std::cout << "[Server] Stopped.\n";
    return 0;
}
