#pragma once
// ============================================================
//  Audio.h  –  音訊 I/O 包裝層（基於 miniaudio）
//  負責：麥克風收音、喇叭播放、VAD（聲音活動偵測）
// ============================================================
#include <cstdint>
#include <functional>
#include <string>

namespace VoIP {

// 音訊參數常數（與 Opus 最佳設定一致）
constexpr int SAMPLE_RATE    = 48000;   // Hz
constexpr int CHANNELS       = 1;       // 單聲道
constexpr int FRAME_MS       = 20;      // 每幀時長（ms）
constexpr int FRAME_SAMPLES  = SAMPLE_RATE * FRAME_MS / 1000; // 960

// 收到麥克風 PCM 資料時的回呼
// pcm: int16 樣本陣列, sampleCount: 樣本數
using CaptureCallback = std::function<void(const int16_t* pcm, int sampleCount)>;

class AudioDevice {
public:
    AudioDevice();
    ~AudioDevice();

    // 初始化音訊裝置
    bool init();
    void shutdown();

    // 開始/停止麥克風收音
    bool startCapture(CaptureCallback cb);
    void stopCapture();

    // 播放 PCM 資料（int16，SAMPLE_RATE，CHANNELS）
    void play(const int16_t* pcm, int sampleCount);

    // VAD：簡易能量閾值偵測，true = 有聲音
    static bool detectVoice(const int16_t* pcm, int sampleCount, float thresholdDb = -40.0f);

private:
    struct Impl;
    Impl* m_impl = nullptr;
};

} // namespace VoIP
