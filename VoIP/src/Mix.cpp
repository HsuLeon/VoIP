// Mix.cpp – 多路 PCM 混音器實作
#include "VoIP/Mix.h"
#include <map>
#include <mutex>
#include <algorithm>
#include <cstring>
#include <cstdint>

namespace VoIP {

struct Mixer::Impl {
    std::map<std::string, std::vector<int16_t>> streams;
    std::mutex mtx;
    int sampleCount;
};

Mixer::Mixer(int sampleCount) : m_impl(new Impl) { m_impl->sampleCount = sampleCount; }
Mixer::~Mixer() { delete m_impl; }

void Mixer::push(const std::string& playerId, const int16_t* pcm, int sampleCount) {
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    auto& buf = m_impl->streams[playerId];
    buf.assign(pcm, pcm + sampleCount);
}

void Mixer::remove(const std::string& playerId) {
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    m_impl->streams.erase(playerId);
}

void Mixer::mix(int16_t* out) {
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    int n = m_impl->sampleCount;
    std::fill(out, out + n, int16_t(0));
    if (m_impl->streams.empty()) return;

    for (int i = 0; i < n; ++i) {
        int32_t sum = 0;
        for (auto& [id, buf] : m_impl->streams)
            if (i < static_cast<int>(buf.size())) sum += buf[i];
        // Clamp 至 int16 範圍
        out[i] = static_cast<int16_t>(
            std::clamp(sum, static_cast<int32_t>(-32768), static_cast<int32_t>(32767)));
    }

    // Each pushed frame represents one 20 ms playout interval.
    // Clearing after mix prevents a transient impulse from being replayed forever
    // when no fresh RTP frame arrives on the next tick.
    for (auto& [id, buf] : m_impl->streams)
        buf.clear();
}

void Mixer::reset() {
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    m_impl->streams.clear();
}

} // namespace VoIP
