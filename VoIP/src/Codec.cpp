// Codec.cpp – Opus 編解碼實作
// 使用 libopus（BSD-3-Clause）
// 官方 Windows 預編譯：https://opus-codec.org/downloads/
//
// TODO 步驟：
//   1. 下載 opus-1.x.x-win-x64.zip，解壓至 third_party/opus/
//   2. 實作 Encoder::init() 呼叫 opus_encoder_create()
//   3. 實作 Encoder::encode() 呼叫 opus_encode()
//   4. 實作 Decoder::init() 呼叫 opus_decoder_create()
//   5. 實作 Decoder::decode() 呼叫 opus_decode()

#include "VoIP/Codec.h"
#include <opus/opus.h>

namespace VoIP {

struct Encoder::Impl {
    OpusEncoder* encoder = nullptr;
};

Encoder::Encoder()  : m_impl(new Impl) {}
Encoder::~Encoder() { shutdown(); delete m_impl; }

bool Encoder::init(int sampleRate, int channels, int bitrate) {
    int err = 0;
    // OPUS_APPLICATION_VOIP：針對語音通話最佳化，內建降噪與舒適噪音（CN）
    m_impl->encoder = opus_encoder_create(sampleRate, channels, OPUS_APPLICATION_VOIP, &err);
    if (err != OPUS_OK || !m_impl->encoder) return false;
    opus_encoder_ctl(m_impl->encoder, OPUS_SET_BITRATE(bitrate));
    opus_encoder_ctl(m_impl->encoder, OPUS_SET_DTX(1));        // 恢復 DTX：靜音幀不送封包，省頻寬
    opus_encoder_ctl(m_impl->encoder, OPUS_SET_INBAND_FEC(1)); // 前向錯誤糾正
    return true;
}

void Encoder::shutdown() {
    if (m_impl->encoder) { opus_encoder_destroy(m_impl->encoder); m_impl->encoder = nullptr; }
}

int Encoder::encode(const int16_t* pcm, int sampleCount, uint8_t* out, int maxOutBytes) {
    if (!m_impl->encoder) return -1;
    return opus_encode(m_impl->encoder, pcm, sampleCount, out, maxOutBytes);
}

// ─────────────────────────────────────────────────────

struct Decoder::Impl {
    OpusDecoder* decoder = nullptr;
};

Decoder::Decoder()  : m_impl(new Impl) {}

// 移動建構/賦值：轉移 m_impl 所有權，moved-from 物件歸零
// 修正 PeerConn std::move 後 double-delete JitterBuffer::Impl 的 crash
Decoder::Decoder(Decoder&& o) noexcept : m_impl(o.m_impl) {
    o.m_impl = nullptr;
}

Decoder& Decoder::operator=(Decoder&& o) noexcept {
    if (this != &o) {
        if (m_impl) { shutdown(); delete m_impl; }
        m_impl   = o.m_impl;
        o.m_impl = nullptr;
    }
    return *this;
}

Decoder::~Decoder() {
    if (m_impl) { shutdown(); delete m_impl; } // nullptr guard：moved-from 安全
}

bool Decoder::init(int sampleRate, int channels) {
    int err = 0;
    m_impl->decoder = opus_decoder_create(sampleRate, channels, &err);
    return (err == OPUS_OK && m_impl->decoder);
}

void Decoder::shutdown() {
    if (!m_impl) return;
    if (m_impl->decoder) { opus_decoder_destroy(m_impl->decoder); m_impl->decoder = nullptr; }
}

int Decoder::decode(const uint8_t* data, int dataLen, int16_t* out, int maxSamples) {
    if (!m_impl || !m_impl->decoder) return -1;
    return opus_decode(m_impl->decoder, data, dataLen, out, maxSamples, 0);
}

int Decoder::decodeFec(int16_t* out, int maxSamples) {
    if (!m_impl || !m_impl->decoder) return -1;
    // 傳入 nullptr 表示封包遺失，啟用 PLC
    return opus_decode(m_impl->decoder, nullptr, 0, out, maxSamples, 1);
}

} // namespace VoIP
