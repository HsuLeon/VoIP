#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "VoIP/VoIP.h"

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
    VoIP::IpcOptions ipc;
};

void printUsage() {
    std::cerr << "Usage:\n";
    std::cerr << "  GameClientMock.exe --ipc-type socket [--ipc-port 17832]\n";
    std::cerr << "  GameClientMock.exe --ipc-type namedPipe [--ipc-name RanOnlineVoIP]\n";
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
        if (i + 1 >= argc) {
            continue;
        }

        const std::string val = argv[i + 1];
        if (key == "--ipc-type") {
            if (!parseIpcType(val, args.ipc.transport)) {
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
                    args.ipc.socketPort = static_cast<uint16_t>(port);
                }
            } catch (...) {
            }
            ++i;
            continue;
        }
        if (key == "--ipc-name") {
            args.ipc.pipeName = val;
            ++i;
            continue;
        }
    }
    return args;
}

std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char ch : s) {
        switch (ch) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out.push_back(ch); break;
        }
    }
    return out;
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
    std::cout << "  IPC type : " << transportToText(args.ipc.transport) << "\n";
    if (args.ipc.transport == VoIP::IpcTransport::Socket) {
        std::cout << "  Target   : 127.0.0.1:" << args.ipc.socketPort << "\n";
    } else {
        std::cout << "  Target   : \\\\.\\pipe\\" << args.ipc.pipeName << "\n";
    }
    std::cout << "========================================\n";

    VoIP::IpcClient ipc;
    if (!ipc.connect(args.ipc, [](const std::string& json) {
            std::cout << "[IPC-EVT] " << json << "\n";
        })) {
        std::cerr << "[ERR] Could not connect to VoIP Client IPC.\n";
        return 1;
    }

    ipc.send("{\"ver\":1,\"cmd\":\"HELLO\"}");
    ipc.send("{\"ver\":1,\"cmd\":\"STATUS\"}");

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
            ipc.send("{\"ver\":1,\"cmd\":\"HELLO\"}");
            continue;
        }
        if (cmd == "status") {
            ipc.send("{\"ver\":1,\"cmd\":\"STATUS\"}");
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

            std::string json =
                "{\"ver\":1,\"cmd\":\"LOGIN\",\"playerId\":\"" +
                jsonEscape(playerId) + "\",\"token\":\"" + jsonEscape(token) + "\"";
            if (!server.empty()) {
                json += ",\"server\":\"" + jsonEscape(server) + "\"";
            }
            if (!channelId.empty()) {
                json += ",\"channelId\":\"" + jsonEscape(channelId) + "\"";
            }
            json += "}";
            ipc.send(json);
            continue;
        }
        if (cmd == "join") {
            std::string channelId;
            iss >> channelId;
            std::string json = "{\"ver\":1,\"cmd\":\"JOIN\"";
            if (!channelId.empty()) {
                json += ",\"channelId\":\"" + jsonEscape(channelId) + "\"";
            }
            json += "}";
            ipc.send(json);
            continue;
        }
        if (cmd == "leave") {
            ipc.send("{\"ver\":1,\"cmd\":\"LEAVE\"}");
            continue;
        }
        if (cmd == "mute") {
            std::string arg;
            iss >> arg;
            if (arg == "on") {
                ipc.send("{\"ver\":1,\"cmd\":\"MUTE\",\"muted\":true}");
            } else if (arg == "off") {
                ipc.send("{\"ver\":1,\"cmd\":\"MUTE\",\"muted\":false}");
            } else {
                std::cout << "Usage: mute on|off\n";
            }
            continue;
        }
        if (cmd == "toggle-mute") {
            ipc.send("{\"ver\":1,\"cmd\":\"TOGGLE_MUTE\"}");
            continue;
        }
        if (cmd == "quit-client") {
            ipc.send("{\"ver\":1,\"cmd\":\"QUIT\"}");
            continue;
        }
        if (cmd == "quit" || cmd == "exit") {
            break;
        }

        std::cout << "Unknown command: " << cmd << "\n";
        printHelp();
    }

    ipc.disconnect();
    return 0;
}
