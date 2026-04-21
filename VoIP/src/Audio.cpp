// Audio.cpp - audio I/O built on top of miniaudio.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include "VoIP/Audio.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <mutex>

namespace VoIP {

struct AudioDevice::Impl {
    ma_device        captureDevice{};
    ma_device_config captureConfig{};
    CaptureCallback  captureCallback;
    bool             captureInitialized = false;

    ma_device        playbackDevice{};
    ma_device_config playbackConfig{};
    bool             playbackInitialized = false;

    std::deque<int16_t> playQueue;
    std::mutex          playMtx;
    std::mutex          deviceMtx;
    std::atomic<bool>   captureRestartPending{false};
    std::atomic<bool>   playbackRestartPending{false};
    std::atomic<uint64_t> captureGeneration{0};
    std::atomic<uint64_t> playbackGeneration{0};

    static void maCaptureCb(ma_device* pDevice,
                            void* pOutput,
                            const void* pInput,
                            ma_uint32 frameCount)
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

    static void maPlaybackCb(ma_device* pDevice,
                             void* pOutput,
                             const void* pInput,
                             ma_uint32 frameCount)
    {
        (void)pInput;
        auto* impl   = static_cast<Impl*>(pDevice->pUserData);
        auto* out    = static_cast<int16_t*>(pOutput);
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
                out[i] = 0;
            }
        }
    }

    static void maNotificationCb(const ma_device_notification* pNotification)
    {
        if (!pNotification || !pNotification->pDevice) return;

        auto* impl = static_cast<Impl*>(pNotification->pDevice->pUserData);
        if (!impl) return;

        const bool shouldRestart =
            pNotification->type == ma_device_notification_type_rerouted ||
            pNotification->type == ma_device_notification_type_interruption_ended;

        if (!shouldRestart) return;

        if (pNotification->pDevice->type == ma_device_type_capture) {
            impl->captureRestartPending.store(true);
        } else if (pNotification->pDevice->type == ma_device_type_playback) {
            impl->playbackRestartPending.store(true);
        }
    }
};

bool AudioDevice::restartPlaybackDevice(Impl* impl)
{
    if (!impl) return false;

    if (impl->playbackInitialized) {
        ma_device_stop(&impl->playbackDevice);
        ma_device_uninit(&impl->playbackDevice);
        impl->playbackInitialized = false;
    }

    impl->playbackConfig = ma_device_config_init(ma_device_type_playback);
    impl->playbackConfig.playback.format          = ma_format_s16;
    impl->playbackConfig.playback.channels        = static_cast<ma_uint32>(CHANNELS);
    impl->playbackConfig.sampleRate               = static_cast<ma_uint32>(SAMPLE_RATE);
    impl->playbackConfig.periodSizeInMilliseconds = static_cast<ma_uint32>(FRAME_MS);
    impl->playbackConfig.dataCallback             = AudioDevice::Impl::maPlaybackCb;
    impl->playbackConfig.notificationCallback     = AudioDevice::Impl::maNotificationCb;
    impl->playbackConfig.pUserData                = impl;

    if (ma_device_init(nullptr, &impl->playbackConfig, &impl->playbackDevice) != MA_SUCCESS)
        return false;

    if (ma_device_start(&impl->playbackDevice) != MA_SUCCESS) {
        ma_device_uninit(&impl->playbackDevice);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(impl->playMtx);
        impl->playQueue.clear();
    }
    impl->playbackInitialized = true;
    ++impl->playbackGeneration;
    return true;
}

bool AudioDevice::restartCaptureDevice(Impl* impl)
{
    if (!impl) return false;

    if (impl->captureInitialized) {
        ma_device_stop(&impl->captureDevice);
        ma_device_uninit(&impl->captureDevice);
        impl->captureInitialized = false;
    }

    impl->captureConfig = ma_device_config_init(ma_device_type_capture);
    impl->captureConfig.capture.format            = ma_format_s16;
    impl->captureConfig.capture.channels          = static_cast<ma_uint32>(CHANNELS);
    impl->captureConfig.sampleRate                = static_cast<ma_uint32>(SAMPLE_RATE);
    impl->captureConfig.periodSizeInMilliseconds  = static_cast<ma_uint32>(FRAME_MS);
    impl->captureConfig.dataCallback              = AudioDevice::Impl::maCaptureCb;
    impl->captureConfig.notificationCallback      = AudioDevice::Impl::maNotificationCb;
    impl->captureConfig.pUserData                 = impl;

    if (ma_device_init(nullptr, &impl->captureConfig, &impl->captureDevice) != MA_SUCCESS)
        return false;

    if (ma_device_start(&impl->captureDevice) != MA_SUCCESS) {
        ma_device_uninit(&impl->captureDevice);
        return false;
    }

    impl->captureInitialized = true;
    ++impl->captureGeneration;
    return true;
}

void AudioDevice::servicePendingDeviceRestarts(Impl* impl)
{
    if (!impl) return;

    const bool restartPlayback = impl->playbackRestartPending.exchange(false);
    const bool restartCapture  = impl->captureRestartPending.exchange(false);
    if (!restartPlayback && !restartCapture) return;

    std::lock_guard<std::mutex> lock(impl->deviceMtx);

    if (restartPlayback && !restartPlaybackDevice(impl)) {
        impl->playbackRestartPending.store(true);
    }

    if (restartCapture && impl->captureCallback && !restartCaptureDevice(impl)) {
        impl->captureRestartPending.store(true);
    }
}

AudioDevice::AudioDevice() : m_impl(new Impl) {}
AudioDevice::~AudioDevice()
{
    shutdown();
    delete m_impl;
}

bool AudioDevice::init()
{
    std::lock_guard<std::mutex> lock(m_impl->deviceMtx);
    if (m_impl->playbackInitialized) return true;
    return restartPlaybackDevice(m_impl);
}

void AudioDevice::shutdown()
{
    std::lock_guard<std::mutex> lock(m_impl->deviceMtx);

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

bool AudioDevice::startCapture(CaptureCallback cb)
{
    std::lock_guard<std::mutex> lock(m_impl->deviceMtx);
    m_impl->captureCallback = std::move(cb);
    return restartCaptureDevice(m_impl);
}

void AudioDevice::stopCapture()
{
    std::lock_guard<std::mutex> lock(m_impl->deviceMtx);
    if (m_impl->captureInitialized)
        ma_device_stop(&m_impl->captureDevice);
}

void AudioDevice::play(const int16_t* pcm, int sampleCount)
{
    servicePendingDeviceRestarts(m_impl);

    if (!pcm || sampleCount <= 0) return;

    std::lock_guard<std::mutex> lock(m_impl->playMtx);

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

uint64_t AudioDevice::captureGeneration() const
{
    return m_impl->captureGeneration.load();
}

uint64_t AudioDevice::playbackGeneration() const
{
    return m_impl->playbackGeneration.load();
}

bool AudioDevice::detectVoice(const int16_t* pcm, int sampleCount, float thresholdDb)
{
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
