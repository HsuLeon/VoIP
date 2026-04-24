#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "CVoIPClientApp.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>

namespace {

std::atomic<bool> g_running{true};

BOOL WINAPI consoleCtrlHandler(DWORD) {
    g_running = false;
    return TRUE;
}

struct Args {
    std::string serverHost;
    uint16_t serverPort = 7000;
    std::string token;
    std::string channelId;
    std::string playerId;

    bool serverProvided = false;
    bool tokenProvided = false;
    bool channelProvided = false;
    bool playerProvided = false;

    bool ipcRequested = false;
    std::string ipcType;
    uint16_t ipcPort = VoIP::IPC_DEFAULT_SOCKET_PORT;
    std::string ipcName = VoIP::IPC_DEFAULT_PIPE_NAME;
};

void printClientUsage() {
    std::cerr << "Usage:\n";
    std::cerr << "  IPC mode:\n";
    std::cerr << "    VoIPClient.exe --ipc-type socket [--ipc-port 17832]\n";
    std::cerr << "    VoIPClient.exe --ipc-type namedPipe [--ipc-name RanOnlineVoIP]\n";
    std::cerr << "\n";
    std::cerr << "  Direct startup mode:\n";
    std::cerr << "    VoIPClient.exe --server 127.0.0.1:7000 --token test "
                 "--channel guild_123 --player playerA\n";
}

Args parseArgs(int argc, char* argv[]) {
    Args args;

    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i];
        if (i + 1 >= argc) {
            continue;
        }

        const std::string val = argv[i + 1];
        if (key == "--server") {
            const auto colon = val.find(':');
            try {
                if (colon != std::string::npos) {
                    args.serverHost = val.substr(0, colon);
                    args.serverPort =
                        static_cast<uint16_t>(std::stoi(val.substr(colon + 1)));
                } else {
                    args.serverHost = val;
                }
                args.serverProvided = !args.serverHost.empty();
            } catch (...) {
            }
            ++i;
            continue;
        }
        if (key == "--token") {
            args.token = val;
            args.tokenProvided = !args.token.empty();
            ++i;
            continue;
        }
        if (key == "--channel") {
            args.channelId = val;
            args.channelProvided = !args.channelId.empty();
            ++i;
            continue;
        }
        if (key == "--player") {
            args.playerId = val;
            args.playerProvided = !args.playerId.empty();
            ++i;
            continue;
        }
        if (key == "--ipc-type") {
            args.ipcRequested = true;
            args.ipcType = val;
            ++i;
            continue;
        }
        if (key == "--ipc-port") {
            try {
                const int port = std::stoi(val);
                if (port > 0 && port <= 65535) {
                    args.ipcPort = static_cast<uint16_t>(port);
                }
            } catch (...) {
            }
            ++i;
            continue;
        }
        if (key == "--ipc-name") {
            args.ipcName = val;
            ++i;
            continue;
        }
    }

    return args;
}

bool hasCompleteJoinArgs(const Args& args) {
    return args.serverProvided && args.tokenProvided &&
           args.channelProvided && args.playerProvided;
}

void printBanner(const CVoIPClientAppOptions& options) {
    std::cout << "========================================\n";
    std::cout << "  VoIP Client  [TEST BUILD]\n";
    std::cout << "========================================\n";
    if (options.ipcMode) {
        std::cout << "  Mode    : IPC\n";
        std::cout << "  IPC     : "
                  << CVoIPClientApp::transportToText(options.ipcOptions.transport);
        if (options.ipcOptions.transport == VoIP::IpcTransport::Socket) {
            std::cout << " 127.0.0.1:" << options.ipcOptions.socketPort << "\n";
        } else {
            std::cout << " \\\\.\\pipe\\" << options.ipcOptions.pipeName << "\n";
        }
    } else {
        std::cout << "  Mode    : Direct Startup\n";
        std::cout << "  Server  : " << options.initialConfig.signalingServer
                  << ":" << options.initialConfig.signalingPort << "\n";
        std::cout << "  Channel : " << options.initialConfig.channelId << "\n";
        std::cout << "  Player  : " << options.initialConfig.playerId << "\n";
    }
    std::cout << "========================================\n";
    if (options.ipcMode) {
        std::cout << "  Controlled by IPC game client\n";
        std::cout << "  Heartbeat timeout: "
                  << options.ipcHeartbeatTimeoutMs << " ms\n";
    } else {
        std::cout << "  q+Enter = quit\n";
        std::cout << "  m+Enter = toggle mute\n";
    }
    std::cout << "========================================\n\n";
}

} // namespace

int main(int argc, char* argv[]) {
    SetConsoleCtrlHandler(consoleCtrlHandler, TRUE);

    if (argc <= 1) {
        printClientUsage();
        return 1;
    }

    const Args args = parseArgs(argc, argv);
    const bool ipcMode = args.ipcRequested;
    const bool directMode = !args.ipcRequested && hasCompleteJoinArgs(args);

    if (!ipcMode && !directMode) {
        printClientUsage();
        return 1;
    }

    CVoIPClientAppOptions options;
    options.ipcMode = ipcMode;
    options.autoJoinOnStart = directMode;
    options.initialConfig.signalingServer = args.serverHost;
    options.initialConfig.signalingPort = args.serverPort;
    options.initialConfig.channelId = args.channelId;
    options.initialConfig.playerId = args.playerId;
    options.initialConfig.token = args.token;

    if (ipcMode) {
        if (!CVoIPClientApp::parseIpcType(args.ipcType, options.ipcOptions.transport)) {
            std::cerr << "[WARN] Invalid --ipc-type value: " << args.ipcType << "\n";
            printClientUsage();
            return 1;
        }
        options.ipcOptions.socketPort = args.ipcPort;
        options.ipcOptions.pipeName =
            args.ipcName.empty() ? VoIP::IPC_DEFAULT_PIPE_NAME : args.ipcName;
    }

    CVoIPClientApp::applyClientConfigFile(options.initialConfig, "client_config.json");
    printBanner(options);

    CVoIPClientApp app;
    if (!app.start(options)) {
        return 1;
    }

    std::thread inputThread;
    if (!ipcMode) {
        inputThread = std::thread([&]() {
            std::string line;
            while (g_running && std::getline(std::cin, line)) {
                if (line == "m" || line == "M") {
                    app.toggleMuted();
                } else if (line == "q" || line == "Q" || line == "quit") {
                    app.requestQuit();
                    g_running = false;
                    break;
                }
            }
            g_running = false;
        });
    }

    auto nextTick = std::chrono::steady_clock::now();
    while (g_running && app.isRunning()) {
        app.tick();
        nextTick += std::chrono::milliseconds(20);
        std::this_thread::sleep_until(nextTick);
    }

    app.stop();

    if (inputThread.joinable()) {
        inputThread.join();
    }

    return 0;
}
