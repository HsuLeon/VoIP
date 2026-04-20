#pragma once
// ============================================================
//  Network.h  –  STUN 查詢、UDP Socket、RTP 封包
//  負責：查詢公開 IP:Port、UDP 收送、RTP 格式化
// ============================================================
#include <cstdint>
#include <string>
#include <functional>
#include <vector>

namespace VoIP {

// ── RTP 封包結構 ──────────────────────────────────────
#pragma pack(push, 1)
struct RtpHeader {
    uint8_t  cc_x_p_v;   // version(2) padding(1) ext(1) csrc_count(4)
    uint8_t  m_pt;        // marker(1) payload_type(7)
    uint16_t seq;         // 序號（網路位元組順序）
    uint32_t timestamp;   // 時間戳（網路位元組順序）
    uint32_t ssrc;        // 同步來源識別碼
};
#pragma pack(pop)

struct RtpPacket {
    RtpHeader header;
    std::vector<uint8_t> payload; // Opus 位元組

    std::vector<uint8_t> serialize() const;
    static bool parse(const uint8_t* data, int len, RtpPacket& out);
};

// ── STUN 查詢 ─────────────────────────────────────────
struct StunResult {
    std::string publicIp;
    uint16_t    publicPort = 0;
    bool        success    = false;
};

// 向 STUN Server 查詢本機公開 IP:Port
// stunHost: 例如 "stun.l.google.com"，port: 19302
StunResult queryStun(const std::string& stunHost, uint16_t stunPort = 19302);

// ── UDP Socket 包裝 ───────────────────────────────────
using RecvCallback = std::function<void(const uint8_t* data, int len,
                                        const std::string& fromIp, uint16_t fromPort)>;

class UdpSocket {
public:
    UdpSocket();
    ~UdpSocket();

    bool bind(uint16_t localPort = 0); // 0 = 系統自動分配
    void close();

    StunResult queryStun(const std::string& stunHost, uint16_t stunPort = 19302);
    bool send(const uint8_t* data, int len, const std::string& ip, uint16_t port);
    bool startRecv(RecvCallback cb);   // 非同步接收（背景執行緒）
    void stopRecv();

    uint16_t localPort() const;

private:
    struct Impl;
    Impl* m_impl = nullptr;
};

// ── Jitter Buffer ─────────────────────────────────────
class JitterBuffer {
public:
    explicit JitterBuffer(int maxPackets = 10);
    ~JitterBuffer();

    // PeerConn 以 std::move 存入 map 時需要移動語意；
    // 若無自訂 move，編譯器只複製原始指標，
    // moved-from 物件析構時會 delete 同一個 m_impl → use-after-free crash。
    JitterBuffer(JitterBuffer&&) noexcept;
    JitterBuffer& operator=(JitterBuffer&&) noexcept;

    // 禁止複製（m_impl 不支援共享所有權）
    JitterBuffer(const JitterBuffer&)            = delete;
    JitterBuffer& operator=(const JitterBuffer&) = delete;

    void push(const RtpPacket& pkt);
    bool pop(RtpPacket& out);  // false = buffer 為空
    void reset();

private:
    struct Impl;
    Impl* m_impl = nullptr;
};

} // namespace VoIP
