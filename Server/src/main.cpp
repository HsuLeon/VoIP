// Server/src/main.cpp
// VoIP Server – Signaling（TCP:7000）+ TURN Relay（UDP:40000）合併版
//
// Signaling 協定（JSON over TCP，換行分隔）：
//   C→S  {"cmd":"JOIN","channelId":"x","playerId":"x","token":"x",
//          "udpPort":12345,"ssrc":987654321}
//   S→C  {"cmd":"JOINED","relayPort":40000,
//          "peers":[{"playerId":"x","publicIp":"1.2.3.4","publicPort":12345,"ssrc":111}]}
//   S→C  {"cmd":"PEER_JOIN","playerId":"x","publicIp":"..","publicPort":0,"ssrc":0}
//   S→C  {"cmd":"PEER_LEAVE","playerId":"x"}
//   S→C  {"cmd":"PING"}
//   C→S  {"cmd":"PONG"}
//   C→S  {"cmd":"LEAVE"}
//
// TURN Relay：
//   Client → server UDP:40000（原始 RTP）
//   Server 以 SSRC 辨識發送者頻道，廣播給同頻道其他 Client

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// ═══════════════════════════════════════════════════════════════
//  設定常數
// ═══════════════════════════════════════════════════════════════
constexpr uint16_t SIGNALING_PORT    = 7000;
constexpr uint16_t RELAY_PORT        = 40000;
constexpr int      PING_INTERVAL_MS  = 30000; // 每 30s 送一次 PING
constexpr int      PONG_TIMEOUT_MS   = 60000; // 60s 未 PONG 視為斷線

std::atomic<bool> g_running{true};

// ═══════════════════════════════════════════════════════════════
//  輕量 JSON 工具（與 Channel.cpp 相同實作）
// ═══════════════════════════════════════════════════════════════
static std::string jStr(const std::string& j, const std::string& key) {
    std::string pat = "\"" + key + "\"";
    auto k = j.find(pat);
    if (k == std::string::npos) return {};
    auto c = j.find(':', k + pat.size());
    if (c == std::string::npos) return {};
    auto q1 = j.find('"', c + 1);
    if (q1 == std::string::npos) return {};
    auto q2 = j.find('"', q1 + 1);
    if (q2 == std::string::npos) return {};
    return j.substr(q1 + 1, q2 - q1 - 1);
}

static int jInt(const std::string& j, const std::string& key, int def = 0) {
    std::string pat = "\"" + key + "\"";
    auto k = j.find(pat);
    if (k == std::string::npos) return def;
    auto c = j.find(':', k + pat.size());
    if (c == std::string::npos) return def;
    size_t v = c + 1;
    while (v < j.size() && (j[v] == ' ' || j[v] == '\t' || j[v] == '\r')) ++v;
    if (v >= j.size()) return def;
    char* end;
    long val = strtol(j.c_str() + v, &end, 10);
    return (end > j.c_str() + v) ? static_cast<int>(val) : def;
}

static bool isJsonRawValue(const std::string& v) {
    if (v.empty()) return false;
    if (v == "true" || v == "false" || v == "null") return true;
    if (v[0] == '[' || v[0] == '{') return true;

    size_t i = 0;
    if (v[i] == '-') ++i;
    bool hasDigits = false;
    bool hasDot    = false;
    for (; i < v.size(); ++i) {
        unsigned char ch = static_cast<unsigned char>(v[i]);
        if (isdigit(ch)) {
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

static std::string jBuild(std::vector<std::pair<std::string, std::string>> fields) {
    std::string s = "{";
    for (size_t i = 0; i < fields.size(); ++i) {
        if (i) s += ',';
        s += '"' + fields[i].first + "\":";
        const std::string& v = fields[i].second;
        bool isRaw = isJsonRawValue(v);
        if (isRaw) s += v; else s += '"' + v + '"';
    }
    return s + '}';
}

// ═══════════════════════════════════════════════════════════════
//  RTP SSRC 快速解析（不依賴 VoIP.h）
//  RTP header: v(2) p(1) x(1) cc(4) | m(1) pt(7) | seq(16) | ts(32) | ssrc(32)
// ═══════════════════════════════════════════════════════════════
static bool parseRtpSsrc(const uint8_t* data, int len, uint32_t& ssrcOut) {
    if (len < 12) return false;
    if ((data[0] & 0xC0) != 0x80) return false; // version 必須為 2
    ssrcOut = (uint32_t(data[8])  << 24) | (uint32_t(data[9])  << 16)
            | (uint32_t(data[10]) <<  8) |  uint32_t(data[11]);
    return true;
}

// ═══════════════════════════════════════════════════════════════
//  ClientSession – 每個已連線玩家的狀態
// ═══════════════════════════════════════════════════════════════
struct ClientSession {
    SOCKET      sock        = INVALID_SOCKET;
    std::string playerId;
    std::string channelId;
    uint16_t    localUdpPort = 0;
    std::string lanIp;
    std::string publicIp;
    uint16_t    publicPort  = 0;
    uint32_t    ssrc        = 0;
#if 0
    std::string publicIp;    // 從 TCP getpeername() 取得
    uint16_t    publicPort  = 0;  // 從 JOIN.udpPort 取得（本機 port，可能與外部 NAT port 不同）
    uint32_t    ssrc        = 0;  // 從 JOIN.ssrc 取得
#endif

    // TURN relay 學習的實際外部 UDP 地址
    // 首次從此 Client 收到 relay 封包時自動填入，比 publicPort 更準確
    std::mutex   udpAddrMtx;
    std::string  udpExtIp;
    uint16_t     udpExtPort = 0;
    bool         udpAddrKnown = false;

    // 取得 relay 目標 port（優先使用學習到的外部地址）
    uint16_t relayTargetPort() const {
        return udpAddrKnown ? udpExtPort : publicPort;
    }
    const std::string& relayTargetIp() const {
        return udpAddrKnown ? udpExtIp : publicIp;
    }

    std::atomic<bool>        alive{true};
    std::chrono::steady_clock::time_point lastPong;

    std::mutex  sendMtx;

    // 發送一行 JSON（含 '\n'），失敗時標記 alive=false
    bool sendLine(const std::string& msg) {
        std::lock_guard<std::mutex> lk(sendMtx);
        std::string line = msg + '\n';
        const char* ptr  = line.c_str();
        int total = static_cast<int>(line.size());
        int sent  = 0;
        while (sent < total) {
            int n = ::send(sock, ptr + sent, total - sent, 0);
            if (n <= 0) { alive = false; return false; }
            sent += n;
        }
        return true;
    }
};

// ═══════════════════════════════════════════════════════════════
//  全域 Session 表
// ═══════════════════════════════════════════════════════════════
std::mutex g_sessionsMtx;
// playerId → session（只含已完成 JOIN 的連線）
std::map<std::string, std::shared_ptr<ClientSession>> g_sessions;

// ── 取出同頻道的所有 session（呼叫前不必持有鎖）────────────
static std::vector<std::shared_ptr<ClientSession>>
    getChannelPeers(const std::string& channelId,
                    const std::string& excludePlayerId = "")
{
    std::vector<std::shared_ptr<ClientSession>> result;
    std::lock_guard<std::mutex> lk(g_sessionsMtx);
    for (auto& [pid, s] : g_sessions) {
        if (s->channelId == channelId && pid != excludePlayerId && s->alive)
            result.push_back(s);
    }
    return result;
}

// ── 廣播給同頻道其他成員 ─────────────────────────────────────
static void broadcastToChannel(const std::string& channelId,
                                const std::string& excludePlayerId,
                                const std::string& msg)
{
    for (auto& s : getChannelPeers(channelId, excludePlayerId))
        s->sendLine(msg);
}

static void broadcastPeerAddr(const std::shared_ptr<ClientSession>& session,
                              const std::string& excludePlayerId = "")
{
    std::string peerAddrMsg = jBuild({
        {"cmd",        "PEER_ADDR"},
        {"playerId",   session->playerId},
        {"lanIp",      session->lanIp},
        {"lanPort",    std::to_string(session->localUdpPort)},
        {"publicIp",   session->publicIp},
        {"publicPort", std::to_string(session->publicPort)}
    });
    broadcastToChannel(session->channelId, excludePlayerId, peerAddrMsg);
}

// ═══════════════════════════════════════════════════════════════
//  handleClient – 每個 TCP 連線的工作執行緒
// ═══════════════════════════════════════════════════════════════
static void handleClient(SOCKET clientSock, std::string peerIp) {
    // ── Step 1: 等待 JOIN 訊息（10 秒超時）─────────────────
    DWORD tv = 10000;
    setsockopt(clientSock, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&tv), sizeof(tv));

    std::string recvBuf;
    std::string joinLine;
    {
        char buf[4096];
        while (true) {
            int n = recv(clientSock, buf, static_cast<int>(sizeof(buf)) - 1, 0);
            if (n <= 0) { closesocket(clientSock); return; }
            buf[n] = '\0';
            recvBuf += buf;
            auto pos = recvBuf.find('\n');
            if (pos != std::string::npos) {
                joinLine = recvBuf.substr(0, pos);
                recvBuf  = recvBuf.substr(pos + 1);
                break;
            }
        }
    }

    if (jStr(joinLine, "cmd") != "JOIN") {
        std::cout << "[SIG] " << peerIp << " sent unexpected cmd, closing\n";
        closesocket(clientSock);
        return;
    }

    // ── Step 2: 建立 Session ─────────────────────────────
    auto session = std::make_shared<ClientSession>();
    session->sock       = clientSock;
    session->playerId   = jStr(joinLine, "playerId");
    session->channelId  = jStr(joinLine, "channelId");
    session->localUdpPort = static_cast<uint16_t>(jInt(joinLine, "udpPort"));
    session->lanIp      = jStr(joinLine, "lanIp");
    session->publicIp   = jStr(joinLine, "publicIp");
    session->publicPort = static_cast<uint16_t>(jInt(joinLine, "publicPort"));
    if (session->publicIp.empty()) session->publicIp = peerIp;
    if (session->publicPort == 0)  session->publicPort = session->localUdpPort;
    session->ssrc       = static_cast<uint32_t>(jInt(joinLine, "ssrc"));
    session->lastPong   = std::chrono::steady_clock::now();

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

    // ── Step 3: 收集同頻道既有 peers 並回覆 JOINED ───────
    {
        // 建立 peers 陣列（在持有鎖期間快速完成）
        std::string peersArr = "[";
        {
            std::lock_guard<std::mutex> lk(g_sessionsMtx);

            // 若 playerId 重複，移除舊連線
            auto it = g_sessions.find(session->playerId);
            if (it != g_sessions.end()) {
                it->second->alive = false;
                closesocket(it->second->sock);
                g_sessions.erase(it);
            }

            bool first = true;
            for (auto& [pid, s] : g_sessions) {
                if (s->channelId == session->channelId && s->alive) {
                    if (!first) peersArr += ',';
                    peersArr += jBuild({
                        {"playerId",   s->playerId},
                        {"lanIp",      s->lanIp},
                        {"lanPort",    std::to_string(s->localUdpPort)},
                        {"publicIp",   s->publicIp},
                        {"publicPort", std::to_string(s->publicPort)},
                        {"ssrc",       std::to_string(s->ssrc)}
                    });
                    first = false;
                }
            }

            // 注冊新 session（在鎖內完成，避免 PEER_JOIN race）
            g_sessions[session->playerId] = session;
        }
        peersArr += ']';

        std::string joinedMsg = jBuild({
            {"cmd",       "JOINED"},
            {"relayPort", std::to_string(RELAY_PORT)},
            {"peers",     peersArr}
        });

        // 移除 recv 超時
        DWORD noTv = 0;
        setsockopt(clientSock, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&noTv), sizeof(noTv));

        if (!session->sendLine(joinedMsg)) {
            std::lock_guard<std::mutex> lk(g_sessionsMtx);
            g_sessions.erase(session->playerId);
            closesocket(clientSock);
            return;
        }
    }

    // ── Step 4: 廣播 PEER_JOIN 給同頻道其他成員 ──────────
    {
        std::string peerJoinMsg = jBuild({
            {"cmd",        "PEER_JOIN"},
            {"playerId",   session->playerId},
            {"lanIp",      session->lanIp},
            {"lanPort",    std::to_string(session->localUdpPort)},
            {"publicIp",   session->publicIp},
            {"publicPort", std::to_string(session->publicPort)},
            {"ssrc",       std::to_string(session->ssrc)}
        });
        broadcastToChannel(session->channelId, session->playerId, peerJoinMsg);
    }

    std::cout << "[SIG] JOINED player=" << session->playerId << "\n";

    // ── Step 5: 主收訊迴圈（LEAVE / PONG）───────────────
    while (g_running && session->alive) {
        char buf[4096];
        int n = recv(clientSock, buf, static_cast<int>(sizeof(buf)) - 1, 0);
        if (n <= 0) break;  // 斷線

        buf[n] = '\0';
        recvBuf += buf;

        size_t pos;
        while ((pos = recvBuf.find('\n')) != std::string::npos) {
            std::string line = recvBuf.substr(0, pos);
            recvBuf = recvBuf.substr(pos + 1);
            if (line.empty()) continue;

            std::string cmd = jStr(line, "cmd");

            if (cmd == "LEAVE") {
                session->alive = false;
                break;
            } else if (cmd == "PONG") {
                session->lastPong = std::chrono::steady_clock::now();
            }
            // 其他指令目前忽略
        }
    }

    // ── Step 6: 清理（移除 session，廣播 PEER_LEAVE）────
    {
        std::lock_guard<std::mutex> lk(g_sessionsMtx);
        auto it = g_sessions.find(session->playerId);
        if (it != g_sessions.end() && it->second.get() == session.get())
            g_sessions.erase(it);
    }

    std::string leaveMsg = jBuild({
        {"cmd",      "PEER_LEAVE"},
        {"playerId", session->playerId}
    });
    broadcastToChannel(session->channelId, session->playerId, leaveMsg);

    closesocket(clientSock);
    std::cout << "[SIG] LEAVE player=" << session->playerId << "\n";
}

// ═══════════════════════════════════════════════════════════════
//  Signaling Server – TCP listen & accept 迴圈
// ═══════════════════════════════════════════════════════════════
static void runSignalingServer() {
    SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSock == INVALID_SOCKET) {
        std::cerr << "[SIG] socket() failed: " << WSAGetLastError() << "\n";
        g_running = false; return;
    }

    // SO_REUSEADDR：重啟時避免 TIME_WAIT 問題
    int yes = 1;
    setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&yes), sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(SIGNALING_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (::bind(listenSock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::cerr << "[SIG] bind() failed: " << WSAGetLastError() << "\n";
        closesocket(listenSock); g_running = false; return;
    }
    if (listen(listenSock, SOMAXCONN) != 0) {
        std::cerr << "[SIG] listen() failed: " << WSAGetLastError() << "\n";
        closesocket(listenSock); g_running = false; return;
    }

    std::cout << "[SIG] Listening on TCP:" << SIGNALING_PORT << "\n";

    while (g_running) {
        // 設 1 秒超時讓 accept 可被 g_running 中斷
        fd_set fds; FD_ZERO(&fds); FD_SET(listenSock, &fds);
        timeval timeout{1, 0};
        int r = select(0, &fds, nullptr, nullptr, &timeout);
        if (r <= 0) continue;

        sockaddr_in clientAddr{};
        int addrLen = sizeof(clientAddr);
        SOCKET clientSock = accept(listenSock,
                                   reinterpret_cast<sockaddr*>(&clientAddr),
                                   &addrLen);
        if (clientSock == INVALID_SOCKET) continue;

        char ipStr[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &clientAddr.sin_addr, ipStr, sizeof(ipStr));
        std::string peerIp = ipStr;

        std::cout << "[SIG] Accept from " << peerIp << "\n";

        // 每個連線獨立執行緒（已連線數預期極少，遊戲頻道上限通常 < 20 人）
        std::thread(handleClient, clientSock, peerIp).detach();
    }

    closesocket(listenSock);
}

// ═══════════════════════════════════════════════════════════════
//  TURN Relay – UDP:40000 廣播
//  收到 RTP → 解出 SSRC → 找到發送者 session → 廣播給同頻道其他人
// ═══════════════════════════════════════════════════════════════
static void runTurnRelay() {
    SOCKET udpSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udpSock == INVALID_SOCKET) {
        std::cerr << "[TURN] socket() failed: " << WSAGetLastError() << "\n";
        g_running = false; return;
    }

    int yes = 1;
    setsockopt(udpSock, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&yes), sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(RELAY_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (::bind(udpSock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::cerr << "[TURN] bind() failed: " << WSAGetLastError() << "\n";
        closesocket(udpSock); g_running = false; return;
    }

    std::cout << "[TURN] Listening on UDP:" << RELAY_PORT << "\n";

    // 1 秒超時讓迴圈可被 g_running 中斷
    DWORD tv = 1000;
    setsockopt(udpSock, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&tv), sizeof(tv));

    static constexpr int BUF_SIZE = 2048;
    std::vector<uint8_t> buf(BUF_SIZE);

    while (g_running) {
        sockaddr_in fromAddr{};
        int fromLen = sizeof(fromAddr);

        int n = recvfrom(udpSock,
                         reinterpret_cast<char*>(buf.data()), BUF_SIZE, 0,
                         reinterpret_cast<sockaddr*>(&fromAddr), &fromLen);
        if (n <= 0) continue;

        // ① 解析 SSRC
        uint32_t ssrc = 0;
        if (!parseRtpSsrc(buf.data(), n, ssrc)) continue;

        // ② 解析發送者真實外部 UDP 地址
        char fromIpStr[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &fromAddr.sin_addr, fromIpStr, sizeof(fromIpStr));
        uint16_t fromPort = ntohs(fromAddr.sin_port);

        // ③ 根據 SSRC 找發送者的 channelId，並更新其外部 UDP 地址
        std::string channelId;
        std::string senderId;
        std::shared_ptr<ClientSession> endpointUpdated;
        {
            std::lock_guard<std::mutex> lk(g_sessionsMtx);
            for (auto& [pid, s] : g_sessions) {
                if (s->ssrc == ssrc && s->alive) {
                    channelId = s->channelId;
                    senderId  = pid;

                    // 學習外部 UDP 地址（只需記錄一次）
                    if (!s->udpAddrKnown || s->publicIp != fromIpStr || s->publicPort != fromPort) {
                        std::lock_guard<std::mutex> ulk(s->udpAddrMtx);
                        s->udpExtIp   = fromIpStr;
                        s->udpExtPort = fromPort;
                        s->udpAddrKnown = true;
                        s->publicIp   = fromIpStr;
                        s->publicPort = fromPort;
                        endpointUpdated = s;
                    }
                    break;
                }
            }
        }
        if (channelId.empty()) continue; // 找不到發送者，丟棄

        // ④ 廣播給同頻道其他成員（raw UDP sendto）
        //    優先使用學習到的外部 UDP 地址，fallback 為 JOIN 宣告的 publicPort
        if (endpointUpdated) broadcastPeerAddr(endpointUpdated, endpointUpdated->playerId);
        auto peers = getChannelPeers(channelId, senderId);
        for (auto& peer : peers) {
            if (!peer->alive) continue;

            std::string  destIp;
            uint16_t     destPort;
            {
                std::lock_guard<std::mutex> ulk(peer->udpAddrMtx);
                destIp   = peer->relayTargetIp();
                destPort = peer->relayTargetPort();
            }
            if (destIp.empty() || destPort == 0) continue;

            sockaddr_in dest{};
            dest.sin_family = AF_INET;
            dest.sin_port   = htons(destPort);
            inet_pton(AF_INET, destIp.c_str(), &dest.sin_addr);

            sendto(udpSock,
                   reinterpret_cast<const char*>(buf.data()), n, 0,
                   reinterpret_cast<sockaddr*>(&dest), sizeof(dest));
        }
    }

    closesocket(udpSock);
}

// ═══════════════════════════════════════════════════════════════
//  Heartbeat – 每 30s 廣播 PING，逾時未 PONG 踢除連線
// ═══════════════════════════════════════════════════════════════
static void runHeartbeat() {
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        auto now = std::chrono::steady_clock::now();

        // 收集所有 session（避免鎖期間 I/O）
        std::vector<std::shared_ptr<ClientSession>> all;
        {
            std::lock_guard<std::mutex> lk(g_sessionsMtx);
            for (auto& [pid, s] : g_sessions)
                if (s->alive) all.push_back(s);
        }

        for (auto& s : all) {
            auto msSinceLastPong = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - s->lastPong).count();

            if (msSinceLastPong >= PONG_TIMEOUT_MS) {
                // 踢除：標記不存活，讓 handleClient 執行緒清理
                std::cout << "[HB] Timeout player=" << s->playerId << "\n";
                s->alive = false;
                closesocket(s->sock); // 讓 recv 立即返回
            } else if (msSinceLastPong >= PING_INTERVAL_MS) {
                s->sendLine("{\"cmd\":\"PING\"}");
            }
        }
    }
}

// ═══════════════════════════════════════════════════════════════
//  Ctrl+C 處理
// ═══════════════════════════════════════════════════════════════
static BOOL WINAPI consoleCtrlHandler(DWORD) {
    std::cout << "\n[Server] Shutting down...\n";
    g_running = false;
    return TRUE;
}

// ═══════════════════════════════════════════════════════════════
//  main
// ═══════════════════════════════════════════════════════════════
int main() {
    SetConsoleCtrlHandler(consoleCtrlHandler, TRUE);

    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::cerr << "[Server] WSAStartup failed\n";
        return 1;
    }

    std::cout << "========================================\n";
    std::cout << "  VoIP Server\n";
    std::cout << "  Signaling : TCP:" << SIGNALING_PORT << "\n";
    std::cout << "  TURN Relay: UDP:" << RELAY_PORT     << "\n";
    std::cout << "========================================\n";

    // 啟動三個背景執行緒
    std::thread sigThread(runSignalingServer);
    std::thread turnThread(runTurnRelay);
    std::thread hbThread(runHeartbeat);

    sigThread.join();
    turnThread.join();
    hbThread.join();

    // 關閉所有 session
    {
        std::lock_guard<std::mutex> lk(g_sessionsMtx);
        for (auto& [pid, s] : g_sessions) {
            s->alive = false;
            closesocket(s->sock);
        }
        g_sessions.clear();
    }

    WSACleanup();
    std::cout << "[Server] Stopped.\n";
    return 0;
}
