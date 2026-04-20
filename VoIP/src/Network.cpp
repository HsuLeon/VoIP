// Network.cpp – STUN 查詢 / UDP Socket / RTP 封包 實作
// 使用 Windows Winsock2（系統內建，ws2_32.lib）
// RFC 5389：STUN  RFC 3550：RTP

// Windows 標頭衝突防護（必須在 winsock2.h 之前）
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "VoIP/Network.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <algorithm>
#include <queue>
#include <mutex>
#include <thread>
#include <atomic>
#include <cstring>
#include <cstdlib>  // rand
#include <cstdio>   // snprintf
#include <stdexcept>

#pragma comment(lib, "ws2_32.lib")

namespace VoIP {

// ═══════════════════════════════════════════════════════════════
//  RTP 封包 serialize / parse
// ═══════════════════════════════════════════════════════════════

std::vector<uint8_t> RtpPacket::serialize() const {
    std::vector<uint8_t> buf(sizeof(RtpHeader) + payload.size());

    // 複製 header 並轉換為網路位元組順序
    RtpHeader net   = header;
    net.seq         = htons(header.seq);
    net.timestamp   = htonl(header.timestamp);
    net.ssrc        = htonl(header.ssrc);
    std::memcpy(buf.data(), &net, sizeof(RtpHeader));

    // 附上 payload（Opus 位元組，已是大端序）
    if (!payload.empty())
        std::memcpy(buf.data() + sizeof(RtpHeader),
                    payload.data(), payload.size());

    return buf;
}

bool RtpPacket::parse(const uint8_t* data, int len, RtpPacket& out) {
    if (!data || len < static_cast<int>(sizeof(RtpHeader)))
        return false;

    RtpHeader net;
    std::memcpy(&net, data, sizeof(RtpHeader));

    out.header           = net;
    out.header.seq       = ntohs(net.seq);
    out.header.timestamp = ntohl(net.timestamp);
    out.header.ssrc      = ntohl(net.ssrc);

    int payloadLen = len - static_cast<int>(sizeof(RtpHeader));
    if (payloadLen > 0) {
        out.payload.assign(data + sizeof(RtpHeader),
                           data + sizeof(RtpHeader) + payloadLen);
    } else {
        out.payload.clear();
    }
    return true;
}

// ═══════════════════════════════════════════════════════════════
//  STUN 查詢（RFC 5389）
//  送出 Binding Request，解析 XOR-MAPPED-ADDRESS
// ═══════════════════════════════════════════════════════════════

namespace {
    constexpr uint32_t STUN_MAGIC_COOKIE     = 0x2112A442u;
    constexpr uint16_t STUN_BINDING_REQUEST  = 0x0001u;
    constexpr uint16_t STUN_BINDING_RESPONSE = 0x0101u;
    constexpr uint16_t STUN_ATTR_XOR_MAPPED  = 0x0020u;
    constexpr uint16_t STUN_ATTR_MAPPED      = 0x0001u;
    constexpr uint16_t STUN_FAMILY_IPV4      = 0x0001u;

    StunResult queryStunWithSocket(SOCKET sock,
                                   const std::string& stunHost,
                                   uint16_t stunPort)
    {
        StunResult result;
        if (sock == INVALID_SOCKET) return result;

        addrinfo hints{};
        hints.ai_family   = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_protocol = IPPROTO_UDP;

        char portStr[8];
        snprintf(portStr, sizeof(portStr), "%u", stunPort);

        addrinfo* res = nullptr;
        if (getaddrinfo(stunHost.c_str(), portStr, &hints, &res) != 0)
            return result;

        DWORD timeout = 2000;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&timeout), sizeof(timeout));

        uint8_t request[20] = {};
        request[0] = 0x00; request[1] = 0x01;
        request[2] = 0x00; request[3] = 0x00;
        request[4] = 0x21; request[5] = 0x12; request[6] = 0xA4; request[7] = 0x42;
        for (int i = 8; i < 20; ++i)
            request[i] = static_cast<uint8_t>(rand() & 0xFF);

        if (sendto(sock,
                   reinterpret_cast<const char*>(request), 20, 0,
                   res->ai_addr, static_cast<int>(res->ai_addrlen)) != 20) {
            freeaddrinfo(res);
            return result;
        }

        uint8_t response[512] = {};
        sockaddr_in fromAddr{};
        int fromLen = static_cast<int>(sizeof(fromAddr));
        int recvLen = recvfrom(sock,
                               reinterpret_cast<char*>(response),
                               static_cast<int>(sizeof(response)),
                               0,
                               reinterpret_cast<sockaddr*>(&fromAddr),
                               &fromLen);
        freeaddrinfo(res);
        if (recvLen < 20) return result;

        uint16_t msgType   = (static_cast<uint16_t>(response[0]) << 8) | response[1];
        uint16_t msgLength = (static_cast<uint16_t>(response[2]) << 8) | response[3];
        if (msgType != STUN_BINDING_RESPONSE) return result;

        int offset = 20;
        int end    = 20 + static_cast<int>(msgLength);
        end = std::min(end, recvLen);

        while (offset + 4 <= end) {
            uint16_t attrType = (static_cast<uint16_t>(response[offset])     << 8)
                               | response[offset + 1];
            uint16_t attrLen  = (static_cast<uint16_t>(response[offset + 2]) << 8)
                               | response[offset + 3];
            offset += 4;

            if ((attrType == STUN_ATTR_XOR_MAPPED || attrType == STUN_ATTR_MAPPED)
                && attrLen >= 8 && offset + 8 <= end)
            {
                uint8_t family = response[offset + 1];
                if (family == STUN_FAMILY_IPV4) {
                    uint16_t xport = (static_cast<uint16_t>(response[offset + 2]) << 8)
                                    | response[offset + 3];
                    uint32_t xaddr = (static_cast<uint32_t>(response[offset + 4]) << 24)
                                   | (static_cast<uint32_t>(response[offset + 5]) << 16)
                                   | (static_cast<uint32_t>(response[offset + 6]) <<  8)
                                   |  static_cast<uint32_t>(response[offset + 7]);

                    uint16_t port;
                    uint32_t addr;
                    if (attrType == STUN_ATTR_XOR_MAPPED) {
                        port = xport ^ static_cast<uint16_t>(STUN_MAGIC_COOKIE >> 16);
                        addr = xaddr ^ STUN_MAGIC_COOKIE;
                    } else {
                        port = xport;
                        addr = xaddr;
                    }

                    result.publicPort = port;

                    uint32_t addrNet = htonl(addr);
                    char ipStr[INET_ADDRSTRLEN] = {};
                    inet_ntop(AF_INET, &addrNet, ipStr, sizeof(ipStr));
                    result.publicIp = ipStr;
                    result.success  = true;
                    break;
                }
            }

            offset += (static_cast<int>(attrLen) + 3) & ~3;
        }

        return result;
    }
} // anonymous namespace

StunResult queryStun(const std::string& stunHost, uint16_t stunPort) {
    StunResult result;

    // 1. 初始化 Winsock
    WSADATA wsaData{};
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
        return result;

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        WSACleanup();
        return result;
    }

    result = queryStunWithSocket(sock, stunHost, stunPort);
    closesocket(sock);
    WSACleanup();
    return result;

    // 2. 解析 STUN 伺服器主機名稱
    addrinfo hints{};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

    char portStr[8];
    snprintf(portStr, sizeof(portStr), "%u", stunPort);

    addrinfo* res = nullptr;
    if (getaddrinfo(stunHost.c_str(), portStr, &hints, &res) != 0) {
        WSACleanup();
        return result;
    }

    // 3. 建立 UDP socket
    SOCKET sockLegacy = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sockLegacy == INVALID_SOCKET) {
        freeaddrinfo(res);
        WSACleanup();
        return result;
    }

    // 設定接收超時 2 秒
    DWORD timeout = 2000;
    setsockopt(sockLegacy, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(timeout));

    // 4. 建構 STUN Binding Request（20 bytes，無 attributes）
    //    格式：type(2) + length(2) + magic_cookie(4) + txId(12)
    uint8_t request[20] = {};
    request[0] = 0x00; request[1] = 0x01; // Binding Request
    request[2] = 0x00; request[3] = 0x00; // Message Length = 0
    // Magic Cookie（big-endian）
    request[4] = 0x21; request[5] = 0x12; request[6] = 0xA4; request[7] = 0x42;
    // Transaction ID：填入偽隨機值
    for (int i = 8; i < 20; ++i)
        request[i] = static_cast<uint8_t>(rand() & 0xFF);

    // 5. 送出請求
    if (sendto(sockLegacy,
               reinterpret_cast<const char*>(request), 20, 0,
               res->ai_addr, static_cast<int>(res->ai_addrlen)) != 20) {
        closesocket(sockLegacy);
        freeaddrinfo(res);
        WSACleanup();
        return result;
    }

    // 6. 接收回應
    uint8_t response[512] = {};
    sockaddr_in fromAddr{};
    int fromLen = static_cast<int>(sizeof(fromAddr));

    int recvLen = recvfrom(sockLegacy,
                           reinterpret_cast<char*>(response),
                           static_cast<int>(sizeof(response)),
                           0,
                           reinterpret_cast<sockaddr*>(&fromAddr),
                           &fromLen);

    closesocket(sockLegacy);
    freeaddrinfo(res);
    WSACleanup();

    if (recvLen < 20) return result;

    // 7. 確認回應類型
    uint16_t msgType   = (static_cast<uint16_t>(response[0]) << 8) | response[1];
    uint16_t msgLength = (static_cast<uint16_t>(response[2]) << 8) | response[3];
    if (msgType != STUN_BINDING_RESPONSE) return result;

    // 8. 掃描 attributes，找 XOR-MAPPED-ADDRESS（或 MAPPED-ADDRESS 作後備）
    int offset = 20; // STUN header = 20 bytes
    int end    = 20 + static_cast<int>(msgLength);
    end = std::min(end, recvLen);

    while (offset + 4 <= end) {
        uint16_t attrType = (static_cast<uint16_t>(response[offset])     << 8)
                           | response[offset + 1];
        uint16_t attrLen  = (static_cast<uint16_t>(response[offset + 2]) << 8)
                           | response[offset + 3];
        offset += 4;

        if ((attrType == STUN_ATTR_XOR_MAPPED || attrType == STUN_ATTR_MAPPED)
            && attrLen >= 8 && offset + 8 <= end)
        {
            // value[0]: reserved
            // value[1]: family
            // value[2-3]: (XOR'd) port
            // value[4-7]: (XOR'd) IP
            uint8_t family = response[offset + 1];
            if (family == STUN_FAMILY_IPV4) {
                uint16_t xport = (static_cast<uint16_t>(response[offset + 2]) << 8)
                                | response[offset + 3];
                uint32_t xaddr = (static_cast<uint32_t>(response[offset + 4]) << 24)
                               | (static_cast<uint32_t>(response[offset + 5]) << 16)
                               | (static_cast<uint32_t>(response[offset + 6]) <<  8)
                               |  static_cast<uint32_t>(response[offset + 7]);

                uint16_t port;
                uint32_t addr;
                if (attrType == STUN_ATTR_XOR_MAPPED) {
                    // XOR-MAPPED-ADDRESS：port XOR (magic >> 16)，IP XOR magic
                    port = xport ^ static_cast<uint16_t>(STUN_MAGIC_COOKIE >> 16);
                    addr = xaddr ^ STUN_MAGIC_COOKIE;
                } else {
                    // MAPPED-ADDRESS：直接使用
                    port = xport;
                    addr = xaddr;
                }

                result.publicPort = port;

                // 轉換為 dotted notation
                uint32_t addrNet = htonl(addr);
                char ipStr[INET_ADDRSTRLEN] = {};
                inet_ntop(AF_INET, &addrNet, ipStr, sizeof(ipStr));
                result.publicIp = ipStr;
                result.success  = true;
                break; // 找到就停止
            }
        }

        // 屬性值對齊 4 bytes
        offset += (static_cast<int>(attrLen) + 3) & ~3;
    }

    return result;
}

// ═══════════════════════════════════════════════════════════════
//  UdpSocket
// ═══════════════════════════════════════════════════════════════

struct UdpSocket::Impl {
    SOCKET             sock      = INVALID_SOCKET;
    uint16_t           localPort = 0;
    std::thread        recvThread;
    std::atomic<bool>  running{false};
    RecvCallback       callback;
};

UdpSocket::UdpSocket()  : m_impl(new Impl) {
    WSADATA wsaData{};
    WSAStartup(MAKEWORD(2, 2), &wsaData);
}

UdpSocket::~UdpSocket() {
    close();
    WSACleanup();
    delete m_impl;
}

bool UdpSocket::bind(uint16_t localPort) {
    if (m_impl->sock != INVALID_SOCKET)
        closesocket(m_impl->sock);

    m_impl->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_impl->sock == INVALID_SOCKET) return false;

    // 允許位址重用
    BOOL reuse = TRUE;
    setsockopt(m_impl->sock, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(localPort); // 0 = 系統自動分配

    if (::bind(m_impl->sock,
               reinterpret_cast<sockaddr*>(&addr),
               sizeof(addr)) != 0) {
        closesocket(m_impl->sock);
        m_impl->sock = INVALID_SOCKET;
        return false;
    }

    // 取得實際綁定的 port
    sockaddr_in bound{};
    int boundLen = sizeof(bound);
    getsockname(m_impl->sock,
                reinterpret_cast<sockaddr*>(&bound), &boundLen);
    m_impl->localPort = ntohs(bound.sin_port);
    return true;
}

void UdpSocket::close() {
    stopRecv();
    if (m_impl->sock != INVALID_SOCKET) {
        closesocket(m_impl->sock);
        m_impl->sock = INVALID_SOCKET;
    }
}

bool UdpSocket::send(const uint8_t* data, int len,
                     const std::string& ip, uint16_t port) {
    if (m_impl->sock == INVALID_SOCKET || !data || len <= 0)
        return false;

    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port   = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &dest.sin_addr) != 1)
        return false;

    int sent = sendto(m_impl->sock,
                      reinterpret_cast<const char*>(data), len, 0,
                      reinterpret_cast<sockaddr*>(&dest), sizeof(dest));
    return sent == len;
}

StunResult UdpSocket::queryStun(const std::string& stunHost, uint16_t stunPort) {
    return queryStunWithSocket(m_impl->sock, stunHost, stunPort);
}

bool UdpSocket::startRecv(RecvCallback cb) {
    if (m_impl->sock == INVALID_SOCKET) return false;
    m_impl->callback = cb;
    m_impl->running  = true;

    m_impl->recvThread = std::thread([this]() {
        uint8_t      buf[2048];
        sockaddr_in  from{};

        while (m_impl->running.load()) {
            int fromLen = static_cast<int>(sizeof(from)); // 每次迴圈重置，recvfrom 會修改此值
            int n = recvfrom(m_impl->sock,
                             reinterpret_cast<char*>(buf),
                             static_cast<int>(sizeof(buf)),
                             0,
                             reinterpret_cast<sockaddr*>(&from),
                             &fromLen);
            if (n <= 0) {
                // socket 被關閉或超時，離開執行緒
                if (!m_impl->running.load()) break;
                continue;
            }

            char ipStr[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &from.sin_addr, ipStr, sizeof(ipStr));

            if (m_impl->callback)
                m_impl->callback(buf, n, ipStr, ntohs(from.sin_port));
        }
    });

    return true;
}

void UdpSocket::stopRecv() {
    m_impl->running = false;
    if (m_impl->sock != INVALID_SOCKET) {
        // 關閉 socket 讓 recvfrom 解除阻塞
        closesocket(m_impl->sock);
        m_impl->sock = INVALID_SOCKET;
    }
    if (m_impl->recvThread.joinable())
        m_impl->recvThread.join();
}

uint16_t UdpSocket::localPort() const { return m_impl->localPort; }

// ═══════════════════════════════════════════════════════════════
//  JitterBuffer（以 RTP 序號排序的最小優先佇列）
// ═══════════════════════════════════════════════════════════════

struct JitterBuffer::Impl {
    // 最小堆：序號最小（最舊）的封包優先取出
    // comp(a, b) = true  →  a 優先級低於 b（a 往底部沉）
    // 我們希望最小 seq 在頂部，所以當 a.seq > b.seq 時 comp=true。
    //
    // ⚠ 必須使用帶符號的 int16_t 差值做比較，才能正確處理序號回繞
    //   且滿足嚴格弱排序（strict weak ordering）。
    //   若改用無符號 uint16_t 差值並與 0x8000 比較，
    //   transitivity（傳遞性）在特定數值組合下會失敗，導致堆結構損壞，
    //   造成 queue.top() 回傳垃圾指標而 crash。
    struct CmpSeq {
        bool operator()(const RtpPacket& a, const RtpPacket& b) const {
            return static_cast<int16_t>(a.header.seq - b.header.seq) > 0;
            //  > 0  → a.seq 比 b.seq「新」（序號較大）→ a 優先級低 → b 先出
        }
    };

    std::priority_queue<RtpPacket, std::vector<RtpPacket>, CmpSeq> queue;
    std::mutex mtx;
    int        maxPackets;
};

JitterBuffer::JitterBuffer(int maxPackets) : m_impl(new Impl) {
    m_impl->maxPackets = maxPackets;
}

// 移動建構：轉移指標所有權，並將 moved-from 物件的指標歸零，
// 避免 moved-from 析構時 double-delete 同一塊記憶體。
JitterBuffer::JitterBuffer(JitterBuffer&& o) noexcept : m_impl(o.m_impl) {
    o.m_impl = nullptr;
}

JitterBuffer& JitterBuffer::operator=(JitterBuffer&& o) noexcept {
    if (this != &o) {
        delete m_impl;          // 釋放自身舊資源
        m_impl   = o.m_impl;    // 接管來源指標
        o.m_impl = nullptr;     // 來源歸零，析構時安全
    }
    return *this;
}

JitterBuffer::~JitterBuffer() { delete m_impl; } // delete nullptr 是合法 no-op

void JitterBuffer::push(const RtpPacket& pkt) {
    if (!m_impl) return; // moved-from 狀態保護
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    // 最小堆：top() 就是最舊封包（seq 最小），直接 pop 即可丟棄最舊
    if (static_cast<int>(m_impl->queue.size()) >= m_impl->maxPackets)
        m_impl->queue.pop(); // 丟棄最舊封包，為新封包騰出空間
    m_impl->queue.push(pkt);
}

bool JitterBuffer::pop(RtpPacket& out) {
    if (!m_impl) return false; // moved-from 狀態保護
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    if (m_impl->queue.empty()) return false;
    out = m_impl->queue.top();
    m_impl->queue.pop();
    return true;
}

void JitterBuffer::reset() {
    if (!m_impl) return; // moved-from 狀態保護
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    while (!m_impl->queue.empty()) m_impl->queue.pop();
}

} // namespace VoIP
