// Audio.cpp – 音訊 I/O 實作
// 使用 miniaudio（單一標頭函式庫，MIT/Public Domain）
// 文件：https://miniaud.io

// Windows 標頭衝突防護（必須在 miniaudio 之前定義）
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include "VoIP/Audio.h"
#include <algorithm>
#include <cmath>
#include <deque>
#include <mutex>

namespace VoIP {

// ── pImpl 完整定義（含 miniaudio 裝置與播放佇列）──────────────
// 兩個 miniaudio callback 定義為 Impl 的 static 成員，
// 如此可直接 cast pUserData 為 Impl* 而不受 private 存取限制。
struct AudioDevice::Impl {
    // 收音裝置（麥克風）
    ma_device        captureDevice{};
    ma_device_config captureConfig{};
    CaptureCallback  captureCallback;
    bool             captureInitialized  = false;

    // 播放裝置（喇叭）
    ma_device        playbackDevice{};
    ma_device_config playbackConfig{};
    bool             playbackInitialized = false;

    // 播放佇列（playback callback 從此取樣本）
    std::deque<int16_t> playQueue;
    std::mutex           playMtx;

    // ── Capture callback（miniaudio 背景執行緒呼叫）──────────
    static void maCaptureCb(ma_device* pDevice,
                             void*       pOutput,
                             const void* pInput,
                             ma_uint32   frameCount)
    {
        (void)pOutput;
        auto* impl = static_cast<Impl*>(pDevice->pUserData);
        if (impl && impl->captureCallback && pInput) {
            impl->captureCallback(
                static_cast<const int16_t*>(pInput),
                static_cast<int>(frameCount) * CHANNELS
            );
        }
    }

    // ── Playback callback（miniaudio 背景執行緒呼叫）────────
    static void maPlaybackCb(ma_device* pDevice,
                              void*       pOutput,
                              const void* pInput,
                              ma_uint32   frameCount)
    {
        (void)pInput;
        auto*  impl   = static_cast<Impl*>(pDevice->pUserData);
        auto*  out    = static_cast<int16_t*>(pOutput);
        size_t needed = static_cast<size_t>(frameCount) * CHANNELS;

        if (!impl) {
            std::fill(out, out + needed, int16_t{0});
            return;
        }

        std::lock_guard<std::mutex> lock(impl->playMtx);
        for (size_t i = 0; i < needed; ++i) {
            if (!impl->playQueue.empty()) {
                out[i] = impl->playQueue.front();
                impl->playQueue.pop_front();
            } else {
                out[i] = 0; // 佇列空時補靜音
            }
        }
    }
};

// ── 建構 / 解構 ───────────────────────────────────────────────
AudioDevice::AudioDevice()  : m_impl(new Impl) {}
AudioDevice::~AudioDevice() { shutdown(); delete m_impl; }

// ── init()：初始化播放裝置（收音裝置由 startCapture 初始化）──
bool AudioDevice::init() {
    if (m_impl->playbackInitialized) return true;

    m_impl->playbackConfig = ma_device_config_init(ma_device_type_playback);
    m_impl->playbackConfig.playback.format          = ma_format_s16;
    m_impl->playbackConfig.playback.channels        = static_cast<ma_uint32>(CHANNELS);
    m_impl->playbackConfig.sampleRate               = static_cast<ma_uint32>(SAMPLE_RATE);
    m_impl->playbackConfig.periodSizeInMilliseconds = static_cast<ma_uint32>(FRAME_MS);
    m_impl->playbackConfig.dataCallback             = AudioDevice::Impl::maPlaybackCb;
    m_impl->playbackConfig.pUserData                = m_impl;

    if (ma_device_init(nullptr, &m_impl->playbackConfig,
                       &m_impl->playbackDevice) != MA_SUCCESS)
        return false;

    if (ma_device_start(&m_impl->playbackDevice) != MA_SUCCESS) {
        ma_device_uninit(&m_impl->playbackDevice);
        return false;
    }

    m_impl->playbackInitialized = true;
    return true;
}

// ── shutdown()：停止並釋放所有裝置 ───────────────────────────
void AudioDevice::shutdown() {
    if (m_impl->captureInitialized) {
        ma_device_stop(&m_impl->captureDevice);
        ma_device_uninit(&m_impl->captureDevice);
        m_impl->captureInitialized = false;
    }
    if (m_impl->playbackInitialized) {
        ma_device_stop(&m_impl->playbackDevice);
        ma_device_uninit(&m_impl->playbackDevice);
        m_impl->playbackInitialized = false;
    }
}

// ── startCapture()：開始麥克風收音 ──────────────────────────
bool AudioDevice::startCapture(CaptureCallback cb) {
    if (m_impl->captureInitialized) {
        ma_device_stop(&m_impl->captureDevice);
        ma_device_uninit(&m_impl->captureDevice);
        m_impl->captureInitialized = false;
    }

    m_impl->captureCallback = cb;

    m_impl->captureConfig = ma_device_config_init(ma_device_type_capture);
    m_impl->captureConfig.capture.format            = ma_format_s16;
    m_impl->captureConfig.capture.channels          = static_cast<ma_uint32>(CHANNELS);
    m_impl->captureConfig.sampleRate                = static_cast<ma_uint32>(SAMPLE_RATE);
    m_impl->captureConfig.periodSizeInMilliseconds  = static_cast<ma_uint32>(FRAME_MS);
    m_impl->captureConfig.dataCallback              = AudioDevice::Impl::maCaptureCb;
    m_impl->captureConfig.pUserData                 = m_impl;

    if (ma_device_init(nullptr, &m_impl->captureConfig,
                       &m_impl->captureDevice) != MA_SUCCESS)
        return false;

    if (ma_device_start(&m_impl->captureDevice) != MA_SUCCESS) {
        ma_device_uninit(&m_impl->captureDevice);
        return false;
    }

    m_impl->captureInitialized = true;
    return true;
}

// ── stopCapture()：停止麥克風（不釋放裝置）──────────────────
void AudioDevice::stopCapture() {
    if (m_impl->captureInitialized)
        ma_device_stop(&m_impl->captureDevice);
}

// ── play()：將 PCM 推入播放佇列 ─────────────────────────────
void AudioDevice::play(const int16_t* pcm, int sampleCount) {
    if (!pcm || sampleCount <= 0) return;

    std::lock_guard<std::mutex> lock(m_impl->playMtx);

    // 防止佇列無限增長：上限 500 ms
    constexpr size_t MAX_QUEUE =
        static_cast<size_t>(SAMPLE_RATE) * CHANNELS / 2;

    size_t newTotal = m_impl->playQueue.size() + static_cast<size_t>(sampleCount);
    if (newTotal > MAX_QUEUE) {
        size_t toRemove = newTotal - MAX_QUEUE;
        for (size_t i = 0; i < toRemove; ++i)
            m_impl->playQueue.pop_front();
    }

    m_impl->playQueue.insert(m_impl->playQueue.end(), pcm, pcm + sampleCount);
}

// ── detectVoice()：能量閾值 VAD ──────────────────────────────
bool AudioDevice::detectVoice(const int16_t* pcm, int sampleCount, float thresholdDb) {
    if (!pcm || sampleCount <= 0) return false;
    double sum = 0.0;
    for (int i = 0; i < sampleCount; ++i)
        sum += static_cast<double>(pcm[i]) * pcm[i];
    double rms = std::sqrt(sum / sampleCount);
    if (rms < 1.0) return false;
    double db = 20.0 * std::log10(rms / 32768.0);
    return db >= static_cast<double>(thresholdDb);
}

} // namespace VoIP
