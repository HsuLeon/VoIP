#pragma once
// ============================================================
//  Mix.h  –  多路 PCM 音訊混音器
//  負責：將多個玩家的 PCM 串流混合成單一輸出
// ============================================================
#include <cstdint>
#include <string>
#include <vector>

namespace VoIP {

class Mixer {
public:
    explicit Mixer(int sampleCount = 960); // 預設 20ms @ 48kHz
    ~Mixer();

    // 加入一路 PCM 串流（以 playerId 區分）
    void push(const std::string& playerId, const int16_t* pcm, int sampleCount);

    // 移除某玩家的串流
    void remove(const std::string& playerId);

    // 混合所有串流，結果寫入 out（長度 = sampleCount）
    // 使用加總後除以路數的平均混音，並做 clamp 防止溢位
    void mix(int16_t* out);

    void reset();

private:
    struct Impl;
    Impl* m_impl = nullptr;
};

} // namespace VoIP
