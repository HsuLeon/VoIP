#pragma once
// ============================================================
//  Codec.h  –  Opus 語音編解碼器包裝層
//  負責：PCM → Opus 壓縮、Opus → PCM 解壓縮
// ============================================================
#include <cstdint>
#include <vector>

namespace VoIP {

// Opus 目標位元率（bps）
constexpr int OPUS_BITRATE_DEFAULT = 24000; // 24 kbps，平衡品質與頻寬

class Encoder {
public:
    Encoder();
    ~Encoder();

    // 初始化編碼器（sampleRate=48000, channels=1）
    bool init(int sampleRate = 48000, int channels = 1, int bitrate = OPUS_BITRATE_DEFAULT);
    void shutdown();

    // 編碼一幀 PCM → Opus 位元組
    // pcm: int16 樣本陣列（長度必須為 FRAME_SAMPLES）
    // out: 輸出 Opus 封包（最大約 1275 bytes）
    // 回傳：編碼後位元組數，<0 表示失敗
    int encode(const int16_t* pcm, int sampleCount, uint8_t* out, int maxOutBytes = 1500);

private:
    struct Impl;
    Impl* m_impl = nullptr;
};

class Decoder {
public:
    Decoder();
    ~Decoder();

    // 同 JitterBuffer：PeerConn 移動時需要正確的移動語意
    Decoder(Decoder&&) noexcept;
    Decoder& operator=(Decoder&&) noexcept;
    Decoder(const Decoder&)            = delete;
    Decoder& operator=(const Decoder&) = delete;

    bool init(int sampleRate = 48000, int channels = 1);
    void shutdown();

    // 解碼 Opus 位元組 → PCM
    // 回傳：解碼後樣本數，<0 表示失敗
    int decode(const uint8_t* data, int dataLen, int16_t* out, int maxSamples);

    // 封包遺失補償（PLC）
    int decodeFec(int16_t* out, int maxSamples);

private:
    struct Impl;
    Impl* m_impl = nullptr;
};

} // namespace VoIP
