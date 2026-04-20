// Client/src/main.cpp
// VoIP Client – 端對端測試版（TEST BUILD）
//
// 本版本跳過 IPC，直接從命令列參數自動 join 頻道，
// 適合兩個 cmd 視窗對打，驗證 Signaling + UDP 打洞 + 音訊管線。
//
// 啟動範例（開兩個 cmd）：
//   VoIPClient.exe --server 127.0.0.1:7000 --token test --channel guild_123 --player playerA
//   VoIPClient.exe --server 127.0.0.1:7000 --token test --channel guild_123 --player playerB
//
// 按 Enter 離開；按 m + Enter 切換靜音

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "VoIP/VoIP.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

// ── Ctrl+C 處理 ───────────────────────────────────────
std::atomic<bool> g_running{true};

BOOL WINAPI consoleCtrlHandler(DWORD) {
    g_running = false;
    return TRUE;
}

// ── 啟動參數解析 ──────────────────────────────────────
struct Args {
    std::string serverHost = "127.0.0.1";
    uint16_t    serverPort = 7000;
    std::string token      = "test";
    std::string channelId  = "guild_123";
    std::string playerId   = "player_A";
};

Args parseArgs(int argc, char* argv[]) {
    Args args;
    for (int i = 1; i + 1 < argc; ++i) {
        std::string key = argv[i];
        std::string val = argv[i + 1];

        if (key == "--server") {
            // 格式：host:port  或  host（預設 7000）
            auto colon = val.find(':');
            if (colon != std::string::npos) {
                args.serverHost = val.substr(0, colon);
                args.serverPort = static_cast<uint16_t>(
                    std::stoi(val.substr(colon + 1)));
            } else {
                args.serverHost = val;
            }
        }
        if (key == "--token")   args.token     = val;
        if (key == "--channel") args.channelId = val;
        if (key == "--player")  args.playerId  = val;
    }
    return args;
}

// ═══════════════════════════════════════════════════════════
//  主程式
// ═══════════════════════════════════════════════════════════
int main(int argc, char* argv[]) {
    SetConsoleCtrlHandler(consoleCtrlHandler, TRUE);

    Args args = parseArgs(argc, argv);

    std::cout << "========================================\n";
    std::cout << "  VoIP Client  [TEST BUILD]\n";
    std::cout << "  Server  : " << args.serverHost << ":" << args.serverPort << "\n";
    std::cout << "  Channel : " << args.channelId  << "\n";
    std::cout << "  Player  : " << args.playerId   << "\n";
    std::cout << "========================================\n";
    std::cout << "  q+Enter = quit\n";
    std::cout << "  m+Enter = toggle mute\n";
    std::cout << "========================================\n\n";

    // ── 建立 Channel 並設定事件回呼 ──────────────────────
    VoIP::Channel channel;

    VoIP::ChannelConfig cfg;
    cfg.signalingServer = args.serverHost;
    cfg.signalingPort   = args.serverPort;
    cfg.channelId       = args.channelId;
    cfg.playerId        = args.playerId;
    cfg.token           = args.token;

    VoIP::ChannelEvents ev;

    ev.onPeerJoined = [](const std::string& id) {
        std::cout << "[+] Peer joined  : " << id << "\n";
    };

    ev.onPeerLeft = [](const std::string& id) {
        std::cout << "[-] Peer left    : " << id << "\n";
    };

#ifdef _DEBUG
    ev.onSpeakingChanged = [](const std::string& id, bool speaking) {
        std::cout << "[~] " << id
                  << (speaking ? " >> SPEAKING" : "    silent")
                  << "\n";
    };
#endif

    ev.onError = [](const std::string& msg) {
        std::cerr << "[ERR] " << msg << "\n";
    };

    // ── 加入頻道 ──────────────────────────────────────────
    std::cout << "[*] Joining channel...\n";
    if (!channel.join(cfg, ev)) {
        std::cerr << "[!] join() failed. Check server is running.\n";
        return 1;
    }
    std::cout << "[*] Joined! Microphone active.\n\n";

    // ── 鍵盤輸入執行緒（Enter/m）────────────────────────
    std::thread inputThread([&]() {
        std::string line;
        while (g_running && std::getline(std::cin, line)) {
            if (line == "m" || line == "M") {
                bool muted = !channel.isMuted();
                channel.setMuted(muted);
                std::cout << (muted ? "[MIC] Muted\n" : "[MIC] Unmuted\n");
            } else if (line == "q" || line == "Q" || line == "quit") {
                // 明確輸入 q 才退出
                g_running = false;
                break;
            }
            // 空行或其他指令直接忽略，不退出
        }
        // getline 回傳 false（EOF / stdin 關閉）時也退出
        g_running = false;
    });

    // ── 主迴圈：每 20ms tick 一次 ────────────────────────
    // tick() 負責：打洞計時、jitter 解碼、VAD、混音、播放
    auto nextTick = std::chrono::steady_clock::now();
    while (g_running) {
        channel.tick();
        nextTick += std::chrono::milliseconds(20);
        std::this_thread::sleep_until(nextTick);
    }

    // ── 離開頻道 ──────────────────────────────────────────
    std::cout << "\n[*] Leaving channel...\n";
    channel.leave();
    std::cout << "[*] Done.\n";

    if (inputThread.joinable())
        inputThread.join();

    return 0;
}
