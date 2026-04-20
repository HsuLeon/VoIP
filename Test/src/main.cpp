// Test/src/main.cpp
// VoIP 單元測試骨架
// 使用 Google Test（未來透過 vcpkg 或手動加入）
//
// 暫時以獨立函式測試核心邏輯，等 Google Test 整合後再改為 TEST() 形式

#include "VoIP/VoIP.h"
#include <iostream>
#include <cassert>
#include <vector>
#include <cstring>

// ── 測試：Opus 編解碼來回 ────────────────────────────
void testOpusRoundtrip() {
    VoIP::Encoder enc;
    VoIP::Decoder dec;

    assert(enc.init());
    assert(dec.init());

    // 產生 960 個靜音樣本（20ms @ 48kHz）
    std::vector<int16_t> input(VoIP::FRAME_SAMPLES, 0);
    std::vector<uint8_t> encoded(1500);
    std::vector<int16_t> decoded(VoIP::FRAME_SAMPLES);

    int encLen = enc.encode(input.data(), VoIP::FRAME_SAMPLES, encoded.data(), 1500);
    assert(encLen > 0);

    int decLen = dec.decode(encoded.data(), encLen, decoded.data(), VoIP::FRAME_SAMPLES);
    assert(decLen == VoIP::FRAME_SAMPLES);

    std::cout << "[PASS] Opus roundtrip: " << encLen << " bytes encoded" << std::endl;
}

// ── 測試：Mixer 混音 ─────────────────────────────────
void testMixer() {
    VoIP::Mixer mixer(VoIP::FRAME_SAMPLES);

    std::vector<int16_t> streamA(VoIP::FRAME_SAMPLES, 1000);
    std::vector<int16_t> streamB(VoIP::FRAME_SAMPLES, 2000);
    std::vector<int16_t> output(VoIP::FRAME_SAMPLES);

    mixer.push("playerA", streamA.data(), VoIP::FRAME_SAMPLES);
    mixer.push("playerB", streamB.data(), VoIP::FRAME_SAMPLES);
    mixer.mix(output.data());

    // 1000 + 2000 = 3000，clamp 後仍為 3000
    assert(output[0] == 3000);
    std::cout << "[PASS] Mixer: output[0] = " << output[0] << std::endl;
}

// ── 測試：VAD 靜音偵測 ───────────────────────────────
void testVad() {
    std::vector<int16_t> silence(VoIP::FRAME_SAMPLES, 0);
    std::vector<int16_t> loud(VoIP::FRAME_SAMPLES, 20000);

    assert(!VoIP::AudioDevice::detectVoice(silence.data(), VoIP::FRAME_SAMPLES));
    assert( VoIP::AudioDevice::detectVoice(loud.data(),    VoIP::FRAME_SAMPLES));
    std::cout << "[PASS] VAD: silence=false, loud=true" << std::endl;
}

// ── 主程式 ────────────────────────────────────────────
int main() {
    std::cout << "=== VoIP Unit Tests ===" << std::endl;
    testOpusRoundtrip();
    testMixer();
    testVad();
    std::cout << "=== All tests passed ===" << std::endl;
    return 0;
}
