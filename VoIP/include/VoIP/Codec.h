#pragma once
#include <cstdint>
#include <vector>

namespace VoIP {

// Slightly higher default bitrate improves remote voice fullness while
// staying conservative for mono VoIP traffic.
constexpr int OPUS_BITRATE_DEFAULT = 32000;

class Encoder {
public:
    Encoder();
    ~Encoder();

    bool init(int sampleRate = 48000, int channels = 1,
              int bitrate = OPUS_BITRATE_DEFAULT);
    void shutdown();

    int encode(const int16_t* pcm, int sampleCount, uint8_t* out,
               int maxOutBytes = 1500);

private:
    struct Impl;
    Impl* m_impl = nullptr;
};

class Decoder {
public:
    Decoder();
    ~Decoder();

    Decoder(Decoder&&) noexcept;
    Decoder& operator=(Decoder&&) noexcept;
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    bool init(int sampleRate = 48000, int channels = 1);
    void shutdown();

    int decode(const uint8_t* data, int dataLen, int16_t* out, int maxSamples);
    int decodeFec(int16_t* out, int maxSamples);

private:
    struct Impl;
    Impl* m_impl = nullptr;
};

} // namespace VoIP
