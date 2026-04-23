#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "CVoIP.h"

#include <atomic>
#include <iostream>
#include <sstream>
#include <string>

namespace {

std::atomic<bool> g_running{true};

BOOL WINAPI consoleCtrlHandler(DWORD) {
    g_running = false;
    return TRUE;
}

struct Args {
    bool valid = false;
    CVoIPOptions voip;
};

void printUsage() {
    std::cerr << "Usage:\n";
    std::cerr << "  GameClientMock.exe --ipc-type socket [--ipc-port 17832] [--show-voip-console|--hide-voip-console]\n";
    std::cerr << "  GameClientMock.exe --ipc-type namedPipe [--ipc-name RanOnlineVoIP] [--show-voip-console|--hide-voip-console]\n";
}

bool parseIpcType(const std::string& text, VoIP::IpcTransport& out) {
    if (text == "socket") {
        out = VoIP::IpcTransport::Socket;
        return true;
    }
    if (text == "namedPipe" || text == "namedpipe") {
        out = VoIP::IpcTransport::NamedPipe;
        return true;
    }
    return false;
}

std::string transportToText(VoIP::IpcTransport transport) {
    return transport == VoIP::IpcTransport::Socket ? "socket" : "namedPipe";
}

Args parseArgs(int argc, char* argv[]) {
    Args args;
    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i];

        if (key == "--show-voip-console") {
            args.voip.showVoipConsole = true;
            continue;
        }
        if (key == "--hide-voip-console") {
            args.voip.showVoipConsole = false;
            continue;
        }

        if (i + 1 >= argc) {
            continue;
        }

        const std::string val = argv[i + 1];
        if (key == "--ipc-type") {
            if (!parseIpcType(val, args.voip.ipc.transport)) {
                return args;
            }
            args.valid = true;
            ++i;
            continue;
        }
        if (key == "--ipc-port") {
            try {
                const int port = std::stoi(val);
                if (port > 0 && port <= 65535) {
                    args.voip.ipc.socketPort = static_cast<uint16_t>(port);
                }
            } catch (...) {
            }
            ++i;
            continue;
        }
        if (key == "--ipc-name") {
            args.voip.ipc.pipeName = val;
            ++i;
            continue;
        }
    }
    return args;
}

void printHelp() {
    std::cout << "Commands:\n";
    std::cout << "  help\n";
    std::cout << "  status\n";
    std::cout << "  hello\n";
    std::cout << "  login <playerId> <token> [server host:port] [channelId]\n";
    std::cout << "  join [channelId]\n";
    std::cout << "  leave\n";
    std::cout << "  mute on|off\n";
    std::cout << "  toggle-mute\n";
    std::cout << "  quit-client\n";
    std::cout << "  quit\n";
}

} // namespace

int main(int argc, char* argv[]) {
    SetConsoleCtrlHandler(consoleCtrlHandler, TRUE);

    if (argc <= 1) {
        printUsage();
        return 1;
    }

    const Args args = parseArgs(argc, argv);
    if (!args.valid) {
        printUsage();
        return 1;
    }

    std::cout << "========================================\n";
    std::cout << "  GameClientMock\n";
    std::cout << "  IPC type : " << transportToText(args.voip.ipc.transport) << "\n";
    if (args.voip.ipc.transport == VoIP::IpcTransport::Socket) {
        std::cout << "  Target   : 127.0.0.1:" << args.voip.ipc.socketPort << "\n";
    } else {
        std::cout << "  Target   : \\\\.\\pipe\\" << args.voip.ipc.pipeName << "\n";
    }
    std::cout << "  VoIP UI  : "
              << (args.voip.showVoipConsole ? "show console" : "hide console") << "\n";
    std::cout << "========================================\n";

    CVoIP voip;
    if (!voip.start(args.voip)) {
        return 1;
    }

    printHelp();
    std::cout << "\n";

    std::string line;
    while (g_running && std::getline(std::cin, line)) {
        std::istringstream iss(line);
        std::string cmd;
        iss >> cmd;

        if (cmd.empty()) continue;

        if (cmd == "help") {
            printHelp();
            continue;
        }
        if (cmd == "hello") {
            voip.hello();
            continue;
        }
        if (cmd == "status") {
            voip.status();
            continue;
        }
        if (cmd == "login") {
            std::string playerId;
            std::string token;
            std::string server;
            std::string channelId;
            iss >> playerId >> token >> server >> channelId;

            if (playerId.empty() || token.empty()) {
                std::cout << "Usage: login <playerId> <token> [server host:port] [channelId]\n";
                continue;
            }

            voip.login(playerId, token, server, channelId);
            continue;
        }
        if (cmd == "join") {
            std::string channelId;
            iss >> channelId;
            voip.join(channelId);
            continue;
        }
        if (cmd == "leave") {
            voip.leave();
            continue;
        }
        if (cmd == "mute") {
            std::string arg;
            iss >> arg;
            if (arg == "on") {
                voip.setMute(true);
            } else if (arg == "off") {
                voip.setMute(false);
            } else {
                std::cout << "Usage: mute on|off\n";
            }
            continue;
        }
        if (cmd == "toggle-mute") {
            voip.toggleMute();
            continue;
        }
        if (cmd == "quit-client") {
            voip.quitClient();
            continue;
        }
        if (cmd == "quit" || cmd == "exit") {
            g_running = false;
            break;
        }

        std::cout << "Unknown command: " << cmd << "\n";
        printHelp();
    }

    voip.stop();
    return 0;
}
