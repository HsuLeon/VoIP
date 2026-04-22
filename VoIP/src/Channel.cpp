// Channel.cpp – 語音頻道連線管理
// 實作：Signaling TCP 連線、UDP 打洞、TURN 中繼切換、RTP 音訊管線
//
// Signaling 協定（JSON over TCP，換行分隔）：
//
// C→S  {"cmd":"JOIN","channelId":"x","playerId":"x","token":"x",
//        "udpPort":12345,"ssrc":987654321}
// S→C  {"cmd":"JOINED","relayPort":40000,
//        "peers":[{"playerId":"x","publicIp":"1.2.3.4","publicPort":12345,"ssrc":111}]}
// S→C  {"cmd":"PEER_JOIN","playerId":"x","publicIp":"..","publicPort":0,"ssrc":0}
// S→C  {"cmd":"PEER_LEAVE","playerId":"x"}
// S→C  {"cmd":"PING"}
// C→S  {"cmd":"PONG"}
// C→S  {"cmd":"LEAVE"}
//
// UDP 打洞：雙方互送 4-byte magic（0x56 6F 49 50 = "VoIP"），
//            3 秒內收不到任何 UDP → 切換 TURN relay
//
// TURN relay：向 serverIp:relayPort 發送原始 RTP，
//              Server 廣播給同頻道其他 Client；
//              收到的 relay 封包同樣是裸 RTP，靠 SSRC 辨識發送者

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

#include "VoIP/Channel.h"
#include "VoIP/Audio.h"
#include "VoIP/Codec.h"
#include "VoIP/Network.h"
#include "VoIP/Mix.h"
#include <rnnoise.h>
#include <speex/speex_echo.h>
#include <speex/speex_preprocess.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace VoIP {

// ═══════════════════════════════════════════════════════════════
//  輕量 JSON 工具（僅支援本協定所需格式）
// ═══════════════════════════════════════════════════════════════
namespace {

// 取出 "key":"value" 的 string 值
std::string jStr(const std::string& j, const std::string& key) {
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

// 取出 "key":number 的整數值
int jInt(const std::string& j, const std::string& key, int def = 0) {
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

// 取出 "key":[{...},{...}] 中的各個 object 字串
std::vector<std::string> jArr(const std::string& j, const std::string& key) {
    std::vector<std::string> result;
    std::string pat = "\"" + key + "\"";
    auto k = j.find(pat);
    if (k == std::string::npos) return result;
    auto bracket = j.find('[', k + pat.size());
    if (bracket == std::string::npos) return result;

    size_t i = bracket + 1;
    while (i < j.size()) {
        // 跳過空白
        while (i < j.size() && (j[i] == ' ' || j[i] == '\t' || j[i] == '\n' || j[i] == ',')) ++i;
        if (i >= j.size() || j[i] == ']') break;
        if (j[i] == '{') {
            int depth = 0;
            size_t start = i;
            for (; i < j.size(); ++i) {
                if (j[i] == '{') ++depth;
                else if (j[i] == '}') { --depth; if (depth == 0) { result.push_back(j.substr(start, i - start + 1)); ++i; break; } }
            }
        } else {
            ++i;
        }
    }
    return result;
}

// 建構 JSON 字串（值若以數字或符號開頭則不加引號）
bool isJsonRawValue(const std::string& v) {
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

std::string getSocketLocalIp(SOCKET sock) {
    sockaddr_in local{};
    int len = sizeof(local);
    if (getsockname(sock, reinterpret_cast<sockaddr*>(&local), &len) != 0)
        return {};
    char ip[INET_ADDRSTRLEN] = {};
    if (!inet_ntop(AF_INET, &local.sin_addr, ip, sizeof(ip)))
        return {};
    return ip;
}

std::string jBuild(std::vector<std::pair<std::string, std::string>> fields) {
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

} // anonymous namespace

// ═══════════════════════════════════════════════════════════════
//  Per-peer 連線狀態
// ═══════════════════════════════════════════════════════════════
struct PeerConn {
    // 識別
    std::string  playerId;
    std::string  lanIp;
    uint16_t     lanPort = 0;
    std::string  publicIp;
    uint16_t     publicPort  = 0;
    std::string  signaledIp;
    uint16_t     signaledPort = 0;
    std::string  lastLearnedIp;
    uint16_t     lastLearnedPort = 0;
    uint32_t     ssrc        = 0;    // 從 Signaling 取得，用於 RTP 辨識

    // 連線模式
    bool         punchDone   = false;
    bool         useTurn     = false;
    std::chrono::steady_clock::time_point punchStart;

    // 音訊解碼
    Decoder      decoder;
    bool         decoderInit = false;
    JitterBuffer jitter{12};

    // VAD / 說話偵測
    bool         speaking      = false;
    int          silentFrames  = 0;     // 連續靜音幀數（10 幀 = 200ms 才切換）
};

// 打洞 magic bytes
static const uint8_t PUNCH_MAGIC[4] = {0x56, 0x6F, 0x49, 0x50}; // "VoIP"

// ═══════════════════════════════════════════════════════════════
//  Channel::Impl
// ═══════════════════════════════════════════════════════════════
struct Channel::Impl {
    // 設定與事件
    ChannelConfig config;
    ChannelEvents events;
    bool          muted   = false;
    bool          joined  = false;

    // Signaling TCP
    SOCKET        sigSock = INVALID_SOCKET;
    std::thread   sigThread;
    std::string   sigBuf;  // TCP 剩餘接收緩衝

    // UDP（RTP direct + TURN relay）
    UdpSocket     udp;
    std::string   serverIp;
    uint16_t      serverRelayPort = 40000;
    std::string   selfLanIp;
    std::string   selfPublicIp;
    uint16_t      selfPublicPort = 0;
    uint16_t      selfLocalUdpPort = 0;

    // 音訊管線
    AudioDevice   audio;
    Encoder       encoder;
    Mixer         mixer{FRAME_SAMPLES};
    SpeexEchoState* echoState = nullptr;
    SpeexPreprocessState* echoPreprocess = nullptr;
    DenoiseState* denoiseState = nullptr;
    std::vector<int16_t> echoBuf;
    std::vector<int16_t> playbackRefBuf;
    int           echoPlaybackFramesQueued = 0;
    int           echoPlaybackFramesFed = 0;
    std::vector<float> denoiseInBuf;
    std::vector<float> denoiseOutBuf;
    uint64_t      lastCaptureGeneration = 0;
    uint64_t      lastPlaybackGeneration = 0;
    int           captureHangoverFrames = 0;
    std::vector<int16_t> captureBuf;   // 收音幀累積緩衝
    std::mutex           captureMtx;
    std::mutex           echoMtx;
    std::mutex           sigSendMtx;
    std::chrono::steady_clock::time_point lastNetworkProbe =
        std::chrono::steady_clock::now();
    std::vector<int16_t> mixBuf = std::vector<int16_t>(FRAME_SAMPLES, 0); // 960 個零元素；in-class initializer 不支援括號語法，改用 = 賦值形式

    // RTP 發送狀態
    uint16_t      seqNum    = 1;
    uint32_t      rtpTs     = 0;
    uint32_t      mySsrc    = 0;

    // Peer 集合
    std::map<std::string, PeerConn> peers;
    mutable std::mutex              peersMtx;

    std::atomic<bool> running{false};

    // ── Debug 計數器（每 5 秒由 tick() 列印）────────────────
    std::atomic<int> dbgCapture{0};   // miniaudio 送進來的幀數（DTX 前）
    std::atomic<int> dbgEncoded{0};   // Opus 編碼成功幀數（len > 0）
    std::atomic<int> dbgSentDirect{0};// RTP 直連發送
    std::atomic<int> dbgSentRelay{0}; // RTP Relay 發送
    std::atomic<int> dbgRecvRtp{0};   // RTP 收到次數（SSRC 比對成功）
    std::atomic<int> dbgUdpRecv{0};   // 原始 UDP 封包收到次數（鎖前，包含 punch magic）
    std::atomic<int> dbgDecoded{0};   // Opus 解碼成功幀數
    std::atomic<int> dbgDecodedPeak{0}; // 最近一次解碼幀的最大絕對值（0=靜音，32767=最大）
    std::chrono::steady_clock::time_point dbgLastPrint =
        std::chrono::steady_clock::now(); // 初始化為現在，避免啟動即觸發

    // ── 捕獲音訊（miniaudio 執行緒呼叫）─────────────────────
    void refreshPeerEndpoint(PeerConn& peer) {
        peer.publicIp   = peer.signaledIp;
        peer.publicPort = peer.signaledPort;
        if (!selfPublicIp.empty() &&
            !selfLanIp.empty() &&
            !peer.lanIp.empty() &&
            peer.lanPort != 0 &&
            peer.signaledIp == selfPublicIp)
        {
            peer.publicIp   = peer.lanIp;
            peer.publicPort = peer.lanPort;
        }
    }

    bool sendSigLine(const std::string& line) {
        if (sigSock == INVALID_SOCKET) return false;
        std::lock_guard<std::mutex> lk(sigSendMtx);
        std::string msg = line + '\n';
        int sent = ::send(sigSock, msg.c_str(), static_cast<int>(msg.size()), 0);
        return sent == static_cast<int>(msg.size());
    }

    void handleNetworkChange() {
        if (!joined || sigSock == INVALID_SOCKET) return;

        const auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(
                now - lastNetworkProbe).count() < 3)
            return;
        lastNetworkProbe = now;

        const std::string newLanIp = getSocketLocalIp(sigSock);
        const uint16_t newLocalUdpPort = udp.localPort();

        StunResult stunResult;
        static constexpr const char* kStunServers[] = {
            "stun.l.google.com",
            "stun.cloudflare.com"
        };
        static constexpr uint16_t kStunPorts[] = {19302, 3478};
        for (size_t i = 0; i < 2; ++i) {
            stunResult = udp.queryStun(kStunServers[i], kStunPorts[i]);
            if (stunResult.success) break;
        }

        const std::string newPublicIp =
            stunResult.success ? stunResult.publicIp : selfPublicIp;
        const uint16_t newPublicPort =
            stunResult.success ? stunResult.publicPort : selfPublicPort;

        if (newLanIp == selfLanIp &&
            newPublicIp == selfPublicIp &&
            newPublicPort == selfPublicPort &&
            newLocalUdpPort == selfLocalUdpPort) {
            return;
        }

        selfLanIp = newLanIp;
        selfPublicIp = newPublicIp;
        selfPublicPort = newPublicPort;
        selfLocalUdpPort = newLocalUdpPort;

        {
            std::lock_guard<std::mutex> lk(peersMtx);
            for (auto& [id, peer] : peers) {
                refreshPeerEndpoint(peer);
                peer.lastLearnedIp.clear();
                peer.lastLearnedPort = 0;
                peer.punchDone = false;
                peer.useTurn = false;
                peer.punchStart = now;
            }
        }

        const std::string updateMsg = jBuild({
            {"cmd",        "UPDATE_ADDR"},
            {"playerId",   config.playerId},
            {"udpPort",    std::to_string(newLocalUdpPort)},
            {"lanIp",      newLanIp},
            {"publicIp",   newPublicIp},
            {"publicPort", std::to_string(newPublicPort)}
        });
        sendSigLine(updateMsg);
    }

    bool initDenoise() {
        shutdownDenoise();
        if (!config.enableDenoise) return true;

        const int rnnoiseFrame = rnnoise_get_frame_size();
        if (rnnoiseFrame <= 0 || (FRAME_SAMPLES % rnnoiseFrame) != 0)
            return false;

        denoiseState = rnnoise_create(nullptr);
        if (!denoiseState) return false;

        denoiseInBuf.resize(static_cast<size_t>(rnnoiseFrame));
        denoiseOutBuf.resize(static_cast<size_t>(rnnoiseFrame));
        return true;
    }

    void shutdownDenoise() {
        if (denoiseState) {
            rnnoise_destroy(denoiseState);
            denoiseState = nullptr;
        }
        denoiseInBuf.clear();
        denoiseOutBuf.clear();
    }

    bool initEchoCancel() {
        shutdownEchoCancel();
        if (!config.enableEchoCancel) return true;

        constexpr int FILTER_MS = 200;
        const int filterLength = SAMPLE_RATE * FILTER_MS / 1000;

        echoState = speex_echo_state_init_mc(
            FRAME_SAMPLES, filterLength, CHANNELS, CHANNELS);
        if (!echoState) return false;

        int sampleRate = SAMPLE_RATE;
        speex_echo_ctl(echoState, SPEEX_ECHO_SET_SAMPLING_RATE, &sampleRate);

        echoPreprocess =
            speex_preprocess_state_init(FRAME_SAMPLES, SAMPLE_RATE);
        if (!echoPreprocess) {
            speex_echo_state_destroy(echoState);
            echoState = nullptr;
            return false;
        }

        int denoise = 0;
        int vad = 0;
        int agc = 0;
        int echoSuppress = -40;
        int echoSuppressActive = -15;
        speex_preprocess_ctl(
            echoPreprocess, SPEEX_PREPROCESS_SET_DENOISE, &denoise);
        speex_preprocess_ctl(
            echoPreprocess, SPEEX_PREPROCESS_SET_VAD, &vad);
        speex_preprocess_ctl(
            echoPreprocess, SPEEX_PREPROCESS_SET_AGC, &agc);
        speex_preprocess_ctl(
            echoPreprocess, SPEEX_PREPROCESS_SET_ECHO_SUPPRESS,
            &echoSuppress);
        speex_preprocess_ctl(
            echoPreprocess, SPEEX_PREPROCESS_SET_ECHO_SUPPRESS_ACTIVE,
            &echoSuppressActive);
        speex_preprocess_ctl(
            echoPreprocess, SPEEX_PREPROCESS_SET_ECHO_STATE, echoState);

        echoBuf.resize(FRAME_SAMPLES);
        return true;
    }

    void shutdownEchoCancel() {
        if (echoPreprocess) {
            speex_preprocess_state_destroy(echoPreprocess);
            echoPreprocess = nullptr;
        }
        if (echoState) {
            speex_echo_state_destroy(echoState);
            echoState = nullptr;
        }
        echoBuf.clear();
        playbackRefBuf.clear();
        echoPlaybackFramesQueued = 0;
        echoPlaybackFramesFed = 0;
    }

    void resetEchoCancel() {
        if (!config.enableEchoCancel) return;
        std::lock_guard<std::mutex> lk(echoMtx);
        shutdownEchoCancel();
        initEchoCancel();
    }

    void registerPlaybackReference(const int16_t* pcm, int samples) {
        if (!echoState || !pcm || samples <= 0) return;
        std::lock_guard<std::mutex> lk(echoMtx);
        if (!echoState) return;
        playbackRefBuf.insert(playbackRefBuf.end(), pcm, pcm + samples);
        while (static_cast<int>(playbackRefBuf.size()) >= FRAME_SAMPLES) {
            speex_echo_playback(echoState, playbackRefBuf.data());
            playbackRefBuf.erase(playbackRefBuf.begin(),
                                 playbackRefBuf.begin() + FRAME_SAMPLES);
            ++echoPlaybackFramesFed;
            if (echoPlaybackFramesFed <= 1)
                continue; // Speex drops the first playback frame as warm-up.
            if (echoPlaybackFramesQueued < 8)
                ++echoPlaybackFramesQueued;
        }
    }

    void applyEchoCancel(int16_t* pcm, int samples) {
        if (!echoState || !pcm || samples != FRAME_SAMPLES) return;
        std::lock_guard<std::mutex> lk(echoMtx);
        if (!echoState || echoBuf.size() != FRAME_SAMPLES) return;
        if (echoPlaybackFramesQueued <= 0) return;
        speex_echo_capture(echoState, pcm, echoBuf.data());
        --echoPlaybackFramesQueued;
        if (echoPreprocess)
            speex_preprocess_run(echoPreprocess, echoBuf.data());
        std::copy(echoBuf.begin(), echoBuf.end(), pcm);
    }

    void applyDenoise(int16_t* pcm, int samples) {
        if (!denoiseState || !pcm || samples <= 0) return;

        const int rnnoiseFrame = rnnoise_get_frame_size();
        if (rnnoiseFrame <= 0 || (samples % rnnoiseFrame) != 0) return;

        for (int offset = 0; offset < samples; offset += rnnoiseFrame) {
            for (int i = 0; i < rnnoiseFrame; ++i)
                denoiseInBuf[static_cast<size_t>(i)] =
                    static_cast<float>(pcm[offset + i]);

            rnnoise_process_frame(denoiseState, denoiseOutBuf.data(),
                                  denoiseInBuf.data());

            for (int i = 0; i < rnnoiseFrame; ++i) {
                float sample = denoiseOutBuf[static_cast<size_t>(i)];
                sample = std::clamp(sample, -32768.0f, 32767.0f);
                pcm[offset + i] = static_cast<int16_t>(std::lround(sample));
            }
        }
    }

    void handleCapture(const int16_t* pcm, int samples) {
        if (muted || !running.load()) return;

        // ① 累積至整幀後編碼（captureMtx 保護，不跨 peersMtx）
        std::vector<std::vector<uint8_t>> rtpBufs;
        {
            std::lock_guard<std::mutex> lk(captureMtx);
            captureBuf.insert(captureBuf.end(), pcm, pcm + samples);
            const uint64_t captureGen = audio.captureGeneration();
            if (captureGen != lastCaptureGeneration) {
                captureBuf.clear();
                captureHangoverFrames = 0;
                lastCaptureGeneration = captureGen;
                resetEchoCancel();
            }

            // 噪音閘門檻（dB）：低於此值的幀視為噪音，直接丟棄不編碼
            // -35 dB ≈ max 音量的 1.8%，可過濾鍵盤聲/風扇聲，保留正常說話聲
            // 若說話時仍被過濾，請在 Windows 設定中提高麥克風增益
            constexpr float NOISE_GATE_DB = -20.0f; // 提高門檻：過濾整合音效卡 100% 增益時的底噪（約 -25dB）
            constexpr float CAPTURE_VAD_DB = -50.0f;
            constexpr int CAPTURE_HANGOVER_FRAMES = 12;

            while (static_cast<int>(captureBuf.size()) >= FRAME_SAMPLES) {
                ++dbgCapture; // 每幀都計數（噪音閘前）

                // ── 軟體噪音閘：低於門檻直接丟棄，不呼叫 Opus encode ─────
                int16_t frame[FRAME_SAMPLES];
                std::copy_n(captureBuf.begin(), FRAME_SAMPLES, frame);
                applyEchoCancel(frame, FRAME_SAMPLES);
                applyDenoise(frame, FRAME_SAMPLES);

                const bool activeNow =
                    AudioDevice::detectVoice(frame, FRAME_SAMPLES,
                                             CAPTURE_VAD_DB);
                if (activeNow) {
                    captureHangoverFrames = CAPTURE_HANGOVER_FRAMES;
                } else if (captureHangoverFrames > 0) {
                    --captureHangoverFrames;
                }

                if (!activeNow && captureHangoverFrames <= 0) {
                    captureBuf.erase(captureBuf.begin(),
                                     captureBuf.begin() + FRAME_SAMPLES);
                    continue; // 噪音幀：不編碼，不發送
                }

                uint8_t opus[1500];
                int len = encoder.encode(frame, FRAME_SAMPLES, opus, 1500);
                captureBuf.erase(captureBuf.begin(),
                                  captureBuf.begin() + FRAME_SAMPLES);

                if (len <= 0) continue; // Opus DTX：靜音時傳回 0，不送封包
                ++dbgEncoded;

                // 建立 RTP 封包
                RtpPacket pkt;
                pkt.header.cc_x_p_v = 0x80;    // version=2
                pkt.header.m_pt     = 0x60;     // payload_type=96（dynamic）
                pkt.header.seq      = seqNum++;
                pkt.header.timestamp = rtpTs;
                pkt.header.ssrc     = mySsrc;
                rtpTs += static_cast<uint32_t>(FRAME_SAMPLES);
                pkt.payload.assign(opus, opus + len);
                rtpBufs.push_back(pkt.serialize());
            }
        }
        if (rtpBufs.empty()) return;

        // ② 發送（peersMtx 保護）
        std::lock_guard<std::mutex> lk(peersMtx);
        for (auto& rtp : rtpBufs) {
            bool sentViaRelay = false;
            for (auto& [id, peer] : peers) {
                if (!peer.useTurn) {
                    udp.send(rtp.data(), static_cast<int>(rtp.size()),
                             peer.publicIp, peer.publicPort);
                    ++dbgSentDirect;
                }
                if ((peer.useTurn || !peer.punchDone) && !sentViaRelay) {
                    // Relay：向 server:relayPort 發送裸 RTP，
                    // server 廣播給同頻道其他成員
                    udp.send(rtp.data(), static_cast<int>(rtp.size()),
                             serverIp, serverRelayPort);
                    sentViaRelay = true;
                    ++dbgSentRelay;
                }
            }
        }
    }

    // ── 接收 UDP（recv 執行緒呼叫）──────────────────────────
    void handleUdpRecv(const uint8_t* data, int len,
                        const std::string& fromIp, uint16_t fromPort)
    {
        if (!running.load() || len < 4) return;
        const bool fromRelay =
            (fromIp == serverIp && fromPort == serverRelayPort);
        ++dbgUdpRecv; // 不需要鎖，在 SSRC 比對前就計數，便於診斷 relay 是否到達

        // 打洞 magic 封包（不是 RTP）
        if (data[0] == PUNCH_MAGIC[0] && data[1] == PUNCH_MAGIC[1] &&
            data[2] == PUNCH_MAGIC[2] && data[3] == PUNCH_MAGIC[3])
        {
            std::lock_guard<std::mutex> lk(peersMtx);
            for (auto& [id, p] : peers) {
                const bool matchesKnownEndpoint =
                    (p.publicIp == fromIp && p.publicPort == fromPort) ||
                    (p.signaledIp == fromIp && p.signaledPort == fromPort) ||
                    (p.lastLearnedIp == fromIp &&
                     p.lastLearnedPort == fromPort);
                if (!fromRelay && matchesKnownEndpoint) {
                    p.lastLearnedIp   = fromIp;
                    p.lastLearnedPort = fromPort;
                    p.publicIp        = fromIp;
                    p.publicPort      = fromPort;
                    p.punchDone       = true;
                    p.useTurn         = false;
                    p.punchStart      = std::chrono::steady_clock::now();
                }
            }
            return;
        }

        // 嘗試解析為 RTP
        RtpPacket pkt;
        if (!RtpPacket::parse(data, len, pkt)) return;

        std::lock_guard<std::mutex> lk(peersMtx);

        // 用 SSRC 找到對應 peer
        PeerConn* peer = nullptr;
        for (auto& [id, p] : peers) {
            if (p.ssrc == pkt.header.ssrc) { peer = &p; break; }
        }
        if (!peer) return;

        // 直連封包 → 確認打洞成功
        if (!fromRelay) {
            peer->lastLearnedIp   = fromIp;
            peer->lastLearnedPort = fromPort;
            peer->publicIp        = fromIp;
            peer->publicPort      = fromPort;
            peer->punchDone       = true;
            peer->useTurn         = false;
            peer->punchStart      = std::chrono::steady_clock::now();
        }

        // 初始化解碼器
        if (!peer->decoderInit)
            peer->decoderInit = peer->decoder.init(SAMPLE_RATE, CHANNELS);

        ++dbgRecvRtp;
        peer->jitter.push(pkt);
    }

    // ── Signaling 接收執行緒 ─────────────────────────────────
    void runSigThread() {
        std::string remaining = sigBuf;
        char buf[4096];

        while (running.load()) {
            int n = recv(sigSock, buf, static_cast<int>(sizeof(buf)) - 1, 0);
            if (n <= 0) {
                if (running.load() && events.onError)
                    events.onError("Signaling disconnected");
                running = false;
                return;
            }
            buf[n] = '\0';
            remaining += buf;

            size_t pos;
            while ((pos = remaining.find('\n')) != std::string::npos) {
                std::string line = remaining.substr(0, pos);
                remaining = remaining.substr(pos + 1);
                if (!line.empty()) processSigMsg(line);
            }
        }
    }

    void processSigMsg(const std::string& line) {
        std::string cmd = jStr(line, "cmd");

        if (cmd == "PEER_JOIN") {
            PeerConn pc;
            pc.playerId   = jStr(line, "playerId");
            pc.lanIp      = jStr(line, "lanIp");
            pc.lanPort    = static_cast<uint16_t>(jInt(line, "lanPort"));
            pc.publicIp   = jStr(line, "publicIp");
            pc.publicPort = static_cast<uint16_t>(jInt(line, "publicPort"));
            pc.signaledIp = pc.publicIp;
            pc.signaledPort = pc.publicPort;
            refreshPeerEndpoint(pc);
            pc.ssrc       = static_cast<uint32_t>(jInt(line, "ssrc"));
            pc.punchStart = std::chrono::steady_clock::now();
            std::string pid = pc.playerId; // move 前先保存，move 後 pc.playerId 會被清空
            {
                std::lock_guard<std::mutex> lk(peersMtx);
                peers[pid] = std::move(pc);
            }
            if (events.onPeerJoined) events.onPeerJoined(pid);

        } else if (cmd == "PEER_ADDR") {
            std::string pid = jStr(line, "playerId");
            std::string lanIp = jStr(line, "lanIp");
            uint16_t lanPort  = static_cast<uint16_t>(jInt(line, "lanPort"));
            std::string ip  = jStr(line, "publicIp");
            uint16_t port   = static_cast<uint16_t>(jInt(line, "publicPort"));
            if (!pid.empty() && !ip.empty() && port != 0) {
                std::lock_guard<std::mutex> lk(peersMtx);
                auto it = peers.find(pid);
                if (it != peers.end()) {
                    if (!lanIp.empty()) it->second.lanIp = lanIp;
                    if (lanPort != 0) it->second.lanPort = lanPort;
                    it->second.signaledIp   = ip;
                    it->second.signaledPort = port;
                    refreshPeerEndpoint(it->second);
                    it->second.lastLearnedIp = ip;
                    it->second.lastLearnedPort = port;
                    it->second.punchDone  = false;
                    it->second.useTurn    = false;
                    it->second.punchStart = std::chrono::steady_clock::now();
                }
            }

        } else if (cmd == "PEER_LEAVE") {
            std::string pid = jStr(line, "playerId");
            {
                std::lock_guard<std::mutex> lk(peersMtx);
                peers.erase(pid);
                mixer.remove(pid);
            }
            if (events.onPeerLeft) events.onPeerLeft(pid);

        } else if (cmd == "PING") {
            // 回應心跳
            sendSigLine("{\"cmd\":\"PONG\"}");
        }
    }
};

// ═══════════════════════════════════════════════════════════════
//  Channel 公開介面
// ═══════════════════════════════════════════════════════════════

Channel::Channel() : m_impl(new Impl) {
    srand(static_cast<unsigned>(time(nullptr)));
    m_impl->mySsrc = (static_cast<uint32_t>(rand()) << 16)
                    | static_cast<uint32_t>(rand());
}

Channel::~Channel() {
    leave();
    m_impl->shutdownEchoCancel();
    m_impl->shutdownDenoise();
    delete m_impl;
}

bool Channel::join(const ChannelConfig& cfg, const ChannelEvents& events) {
    if (m_impl->joined) leave();

    m_impl->config = cfg;
    m_impl->events = events;

    // ── Step 1: 初始化 Winsock（與 UdpSocket 內部各自計數，無衝突）
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);

    // ── Step 2: 初始化音訊與編碼器
    if (!m_impl->audio.init()) {
        if (events.onError) events.onError("Audio init failed");
        WSACleanup(); return false;
    }
    if (!m_impl->encoder.init(SAMPLE_RATE, CHANNELS, OPUS_BITRATE_DEFAULT)) {
        if (events.onError) events.onError("Encoder init failed");
        WSACleanup(); return false;
    }
    if (!m_impl->initDenoise()) {
        if (events.onError) events.onError("RNNoise init failed");
        WSACleanup(); return false;
    }
    if (!m_impl->initEchoCancel()) {
        if (events.onError) events.onError("Speex AEC init failed");
        WSACleanup(); return false;
    }

    // ── Step 3: 綁定本機 UDP（port=0 = 系統自動）
    if (!m_impl->udp.bind(0)) {
        if (events.onError) events.onError("UDP bind failed");
        WSACleanup(); return false;
    }
    uint16_t localUdpPort = m_impl->udp.localPort();

    // ── Step 4: TCP 連線至 Signaling Server
    m_impl->sigSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_impl->sigSock == INVALID_SOCKET) {
        if (events.onError) events.onError("Signaling socket failed");
        WSACleanup(); return false;
    }

    addrinfo hints{}, *res = nullptr;
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char portStr[8];
    snprintf(portStr, sizeof(portStr), "%u", cfg.signalingPort);

    if (getaddrinfo(cfg.signalingServer.c_str(), portStr, &hints, &res) != 0) {
        closesocket(m_impl->sigSock); m_impl->sigSock = INVALID_SOCKET;
        if (events.onError) events.onError("Signaling DNS failed: " + cfg.signalingServer);
        WSACleanup(); return false;
    }

    if (::connect(m_impl->sigSock, res->ai_addr,
                  static_cast<int>(res->ai_addrlen)) != 0) {
        freeaddrinfo(res);
        closesocket(m_impl->sigSock); m_impl->sigSock = INVALID_SOCKET;
        if (events.onError) events.onError("Signaling connect failed");
        WSACleanup(); return false;
    }

    // 記錄 server IP（用於判斷 relay 來源）
    char srvIp[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET,
              &reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr,
              srvIp, sizeof(srvIp));
    m_impl->serverIp = srvIp;
    m_impl->selfLanIp = getSocketLocalIp(m_impl->sigSock);
    freeaddrinfo(res);

    StunResult stunResult;
    static constexpr const char* kStunServers[] = {
        "stun.l.google.com",
        "stun.cloudflare.com"
    };
    static constexpr uint16_t kStunPorts[] = {19302, 3478};
    for (size_t i = 0; i < 2; ++i) {
        stunResult = m_impl->udp.queryStun(kStunServers[i], kStunPorts[i]);
        if (stunResult.success) break;
    }
    m_impl->selfLocalUdpPort = localUdpPort;
    if (stunResult.success) {
        m_impl->selfPublicIp = stunResult.publicIp;
        m_impl->selfPublicPort = stunResult.publicPort;
    } else {
        m_impl->selfPublicPort = localUdpPort;
    }

    // ── Step 5: 送出 JOIN
    std::vector<std::pair<std::string, std::string>> joinFields = {
        {"cmd",       "JOIN"},
        {"channelId", cfg.channelId},
        {"playerId",  cfg.playerId},
        {"token",     cfg.token},
        {"udpPort",   std::to_string(localUdpPort)},
        {"ssrc",      std::to_string(m_impl->mySsrc)}
    };
    if (!m_impl->selfLanIp.empty())
        joinFields.push_back({"lanIp", m_impl->selfLanIp});
    if (stunResult.success) {
        joinFields.push_back({"publicIp", stunResult.publicIp});
        joinFields.push_back({"publicPort", std::to_string(stunResult.publicPort)});
    }
    std::string joinMsg = jBuild(joinFields);

    if (!m_impl->sendSigLine(joinMsg)) {
        closesocket(m_impl->sigSock); m_impl->sigSock = INVALID_SOCKET;
        if (events.onError) events.onError("Signaling JOIN send failed");
        WSACleanup(); return false;
    }

    // ── Step 6: 等待 JOINED 回應（同步讀取第一行）
    std::string firstLine;
    {
        char buf[4096];
        std::string remaining;
        // 設 5 秒超時
        DWORD tv = 5000;
        setsockopt(m_impl->sigSock, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&tv), sizeof(tv));
        while (true) {
            int n = recv(m_impl->sigSock, buf, sizeof(buf) - 1, 0);
            if (n <= 0) {
                closesocket(m_impl->sigSock); m_impl->sigSock = INVALID_SOCKET;
                if (events.onError) events.onError("Signaling JOINED timeout");
                WSACleanup(); return false;
            }
            buf[n] = '\0';
            remaining += buf;
            auto pos = remaining.find('\n');
            if (pos != std::string::npos) {
                firstLine = remaining.substr(0, pos);
                m_impl->sigBuf = remaining.substr(pos + 1);
                break;
            }
        }
        // 移除超時設定
        tv = 0;
        setsockopt(m_impl->sigSock, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&tv), sizeof(tv));
    }

    if (jStr(firstLine, "cmd") != "JOINED") {
        closesocket(m_impl->sigSock); m_impl->sigSock = INVALID_SOCKET;
        if (events.onError) events.onError("Unexpected response: " + firstLine);
        WSACleanup(); return false;
    }

    m_impl->serverRelayPort =
        static_cast<uint16_t>(jInt(firstLine, "relayPort", 40000));

    // 解析既有 peers
    {
        std::lock_guard<std::mutex> lk(m_impl->peersMtx);
        for (auto& obj : jArr(firstLine, "peers")) {
            PeerConn pc;
            pc.playerId   = jStr(obj, "playerId");
            pc.lanIp      = jStr(obj, "lanIp");
            pc.lanPort    = static_cast<uint16_t>(jInt(obj, "lanPort"));
            pc.publicIp   = jStr(obj, "publicIp");
            pc.publicPort = static_cast<uint16_t>(jInt(obj, "publicPort"));
            pc.signaledIp = pc.publicIp;
            pc.signaledPort = pc.publicPort;
            m_impl->refreshPeerEndpoint(pc);
            pc.ssrc       = static_cast<uint32_t>(jInt(obj, "ssrc"));
            pc.punchStart = std::chrono::steady_clock::now();
            if (events.onPeerJoined) events.onPeerJoined(pc.playerId);
            m_impl->peers[pc.playerId] = std::move(pc);
        }
    }

    // ── Step 7: 啟動 UDP 接收
    m_impl->running = true;
    m_impl->udp.startRecv(
        [impl = m_impl](const uint8_t* d, int l,
                         const std::string& ip, uint16_t port) {
            impl->handleUdpRecv(d, l, ip, port);
        });

    // ── Step 8: 啟動 Signaling 接收執行緒
    m_impl->sigThread = std::thread([impl = m_impl]() {
        impl->runSigThread();
    });

    // ── Step 9: 啟動收音（miniaudio 非同步執行緒）
    m_impl->lastPlaybackGeneration = m_impl->audio.playbackGeneration();
    m_impl->audio.setPlaybackTap(
        [impl = m_impl](const int16_t* pcm, int n) {
            impl->registerPlaybackReference(pcm, n);
        });
    m_impl->audio.startCapture(
        [impl = m_impl](const int16_t* pcm, int n) {
            impl->handleCapture(pcm, n);
        });

    m_impl->joined = true;
    return true;
}

void Channel::leave() {
    if (!m_impl->joined) return;
    m_impl->joined  = false;
    m_impl->running = false;

    // 停止收音
    m_impl->audio.stopCapture();
    m_impl->audio.setPlaybackTap({});

    // 送 LEAVE 並關閉 TCP
    if (m_impl->sigSock != INVALID_SOCKET) {
        m_impl->sendSigLine("{\"cmd\":\"LEAVE\"}");
        closesocket(m_impl->sigSock);
        m_impl->sigSock = INVALID_SOCKET;
    }
    if (m_impl->sigThread.joinable()) m_impl->sigThread.join();

    // 停止 UDP
    m_impl->udp.stopRecv();

    // 清除 peers 與 mixer
    {
        std::lock_guard<std::mutex> lk(m_impl->peersMtx);
        m_impl->peers.clear();
        m_impl->mixer.reset();
    }
    {
        std::lock_guard<std::mutex> lk(m_impl->captureMtx);
        m_impl->captureBuf.clear();
    }

    m_impl->audio.shutdown();
    m_impl->shutdownEchoCancel();
    m_impl->shutdownDenoise();
    WSACleanup();
}

void Channel::setMuted(bool muted) { m_impl->muted = muted; }
bool Channel::isMuted() const      { return m_impl->muted; }

std::vector<PeerInfo> Channel::peers() const {
    std::lock_guard<std::mutex> lk(m_impl->peersMtx);
    std::vector<PeerInfo> result;
    for (auto& [id, p] : m_impl->peers) {
        PeerInfo info;
        info.playerId   = p.playerId;
        info.publicIp   = p.publicIp;
        info.publicPort = p.publicPort;
        result.push_back(info);
    }
    return result;
}

void Channel::tick() {
    if (!m_impl->joined) return;

    auto now = std::chrono::steady_clock::now();
    m_impl->handleNetworkChange();
    const uint64_t playbackGen = m_impl->audio.playbackGeneration();
    if (playbackGen != m_impl->lastPlaybackGeneration) {
        m_impl->lastPlaybackGeneration = playbackGen;
        m_impl->resetEchoCancel();
    }

    // ── Debug：每 5 秒列印管線統計 ─────────────────────────
    // 重要：cout 在 Windows CMD 快速編輯模式下可能阻塞（使用者點選視窗選字時）。
    // 若在持有 peersMtx 期間呼叫 cout，會導致 handleCapture / handleUdpRecv
    // 同樣阻塞，整個音訊管線停擺。因此：先在鎖內快照資料，鎖外再印出。
    if (std::chrono::duration_cast<std::chrono::seconds>(
            now - m_impl->dbgLastPrint).count() >= 5)
    {
        m_impl->dbgLastPrint = now;

        // ① 在鎖內快速收集 peer 快照（不呼叫任何 I/O）
        struct PeerSnap { std::string id; bool punchDone, useTurn; uint16_t port; };
        struct PeerRouteSnap {
            std::string id;
            std::string activeIp;
            uint16_t    activePort;
            std::string signaledIp;
            uint16_t    signaledPort;
            std::string learnedIp;
            uint16_t    learnedPort;
            uint32_t    ssrc;
            bool        punchDone;
            bool        useTurn;
            long long   ageMs;
        };
        std::vector<PeerRouteSnap> snap;
        {
            std::lock_guard<std::mutex> lk(m_impl->peersMtx);
            for (auto& [id, p] : m_impl->peers)
                snap.push_back({
                    id,
                    p.publicIp,
                    p.publicPort,
                    p.signaledIp,
                    p.signaledPort,
                    p.lastLearnedIp,
                    p.lastLearnedPort,
                    p.ssrc,
                    p.punchDone,
                    p.useTurn,
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - p.punchStart).count()
                });
        } // ← peersMtx 在此釋放，cout 呼叫不在鎖範圍內

        // ② 鎖外列印（cout 慢或阻塞都不影響其他執行緒）
#ifdef _DEBUG
        std::cout << "[DBG] cap=" << m_impl->dbgCapture.load()
                  << " enc=" << m_impl->dbgEncoded.load()
                  << " direct=" << m_impl->dbgSentDirect.load()
                  << " relay=" << m_impl->dbgSentRelay.load()
                  << " udpRecv=" << m_impl->dbgUdpRecv.load()
                  << " recvRtp=" << m_impl->dbgRecvRtp.load()
                  << " decoded=" << m_impl->dbgDecoded.load()
                  << " peak=" << m_impl->dbgDecodedPeak.load()
                  << " peers=" << snap.size();
        for (auto& ps : snap) {
            const char* route =
                ps.useTurn ? "relay" : (ps.punchDone ? "direct" : "punch");
            std::cout << " [" << ps.id
                      << " route=" << route
                      << " active=" << ps.activeIp << ":" << ps.activePort
                      << " signaled=" << ps.signaledIp << ":" << ps.signaledPort
                      << " learned="
                      << (ps.learnedIp.empty() ? "-" : ps.learnedIp)
                      << ":" << ps.learnedPort
                      << " ssrc=" << ps.ssrc
                      << " punch=" << (ps.punchDone ? "ok" : "wait")
                      << " ageMs=" << ps.ageMs << "]";
        }
        if (snap.empty()) std::cout << " (no peers)";
        std::cout << "\n";
#endif
    }

    // ① 處理每個 peer：打洞計時、jitter buffer 解碼、VAD
    {
        std::lock_guard<std::mutex> lk(m_impl->peersMtx);

        for (auto& [id, peer] : m_impl->peers) {
            // 打洞計時 → 超過 3 秒切換 TURN
            if (!peer.punchDone) {
                auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - peer.punchStart).count();
                if (ms > 3000) {
                    peer.punchDone = true;
                    peer.useTurn   = true;
                } else {
                    // 持續送打洞封包（每次 tick = 20ms，不需要額外限速）
                    m_impl->udp.send(PUNCH_MAGIC, 4,
                                     peer.publicIp, peer.publicPort);
                }
            }

            // 從 jitter buffer 取一幀解碼
            if (peer.useTurn &&
                !peer.publicIp.empty() &&
                peer.publicPort != 0) {
                m_impl->udp.send(PUNCH_MAGIC, 4,
                                 peer.publicIp, peer.publicPort);
            }

            RtpPacket pkt;
            if (!peer.jitter.pop(pkt)) continue;

            if (!peer.decoderInit)
                peer.decoderInit = peer.decoder.init(SAMPLE_RATE, CHANNELS);
            if (!peer.decoderInit) continue;

            int16_t pcm[FRAME_SAMPLES];
            int decoded;
            if (!pkt.payload.empty()) {
                decoded = peer.decoder.decode(
                    pkt.payload.data(), static_cast<int>(pkt.payload.size()),
                    pcm, FRAME_SAMPLES);
            } else {
                // 封包遺失 → PLC
                decoded = peer.decoder.decodeFec(pcm, FRAME_SAMPLES);
            }
            if (decoded <= 0) continue;
            ++m_impl->dbgDecoded;

            // 記錄峰值振幅（用於 debug 確認解碼有無訊號）
            int peak = 0;
            for (int s = 0; s < decoded; ++s) {
                int v = pcm[s] < 0 ? -pcm[s] : pcm[s];
                if (v > peak) peak = v;
            }
            m_impl->dbgDecodedPeak.store(peak);

            m_impl->mixer.push(id, pcm, decoded);

            // VAD：偵測說話狀態變化（防抖：10 幀靜音才切換）
            bool active = AudioDevice::detectVoice(pcm, decoded, -40.0f);
            peer.silentFrames = active ? 0 : peer.silentFrames + 1;
            bool nowSpeaking  = (peer.silentFrames < 10);

            if (nowSpeaking != peer.speaking) {
                peer.speaking = nowSpeaking;
                if (m_impl->events.onSpeakingChanged)
                    m_impl->events.onSpeakingChanged(id, nowSpeaking);
            }
        }
    } // peersMtx released

    // ② 混音並送入播放佇列
    m_impl->mixer.mix(m_impl->mixBuf.data());
    m_impl->audio.play(m_impl->mixBuf.data(), FRAME_SAMPLES);
}

} // namespace VoIP
