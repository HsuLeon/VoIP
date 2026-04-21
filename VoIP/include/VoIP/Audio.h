#pragma once

#include <cstdint>
#include <functional>

namespace VoIP {

constexpr int SAMPLE_RATE   = 48000;
constexpr int CHANNELS      = 1;
constexpr int FRAME_MS      = 20;
constexpr int FRAME_SAMPLES = SAMPLE_RATE * FRAME_MS / 1000;

using CaptureCallback = std::function<void(const int16_t* pcm, int sampleCount)>;

class AudioDevice {
public:
    AudioDevice();
    ~AudioDevice();

    bool init();
    void shutdown();

    bool startCapture(CaptureCallback cb);
    void stopCapture();

    void play(const int16_t* pcm, int sampleCount);

    static bool detectVoice(const int16_t* pcm, int sampleCount, float thresholdDb = -40.0f);

private:
    struct Impl;
    static bool restartPlaybackDevice(Impl* impl);
    static bool restartCaptureDevice(Impl* impl);
    static void servicePendingDeviceRestarts(Impl* impl);

    Impl* m_impl = nullptr;
};

} // namespace VoIP
