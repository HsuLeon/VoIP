#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "VoIP/VoIP.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

std::atomic<bool> g_running{true};
constexpr int HEARTBEAT_INTERVAL_MS = 1000;

BOOL WINAPI consoleCtrlHandler(DWORD) {
    g_running = false;
    return TRUE;
}

struct Args {
    bool valid = false;
    VoIP::IpcOptions ipc;
#ifdef _DEBUG
    bool showVoipConsole = true;
#else
    bool showVoipConsole = false;
#endif
};

struct ChildProcess {
    PROCESS_INFORMATION pi{};
    bool launched = false;
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

std::string makePipePath(const std::string& pipeName, const char* suffix) {
    return "\\\\.\\pipe\\" + pipeName + suffix;
}

bool namedPipeEndpointExists(const VoIP::IpcOptions& ipc) {
    const std::string c2s = makePipePath(ipc.pipeName, ".c2s");
    const std::string s2c = makePipePath(ipc.pipeName, ".s2c");
    return WaitNamedPipeA(c2s.c_str(), 50) || WaitNamedPipeA(s2c.c_str(), 50);
}

std::string executableDirectory() {
    std::vector<char> path(MAX_PATH);
    DWORD len = GetModuleFileNameA(nullptr, path.data(), static_cast<DWORD>(path.size()));
    while (len == path.size()) {
        path.resize(path.size() * 2);
        len = GetModuleFileNameA(nullptr, path.data(), static_cast<DWORD>(path.size()));
    }
    if (len == 0) {
        return ".";
    }

    std::string full(path.data(), len);
    const size_t slash = full.find_last_of("\\/");
    if (slash == std::string::npos) {
        return ".";
    }
    return full.substr(0, slash);
}

std::string quoteArg(const std::string& text) {
    std::string out = "\"";
    for (char ch : text) {
        if (ch == '"') {
            out += "\\\"";
        } else {
            out.push_back(ch);
        }
    }
    out += "\"";
    return out;
}

std::string buildVoipClientCommandLine(const std::string& exePath, const Args& args) {
    std::string cmd = quoteArg(exePath);
    cmd += " --ipc-type ";
    cmd += transportToText(args.ipc.transport);
    if (args.ipc.transport == VoIP::IpcTransport::Socket) {
        cmd += " --ipc-port ";
        cmd += std::to_string(args.ipc.socketPort);
    } else {
        cmd += " --ipc-name ";
        cmd += quoteArg(args.ipc.pipeName);
    }
    return cmd;
}

bool launchVoipClient(const Args& args, ChildProcess& child) {
    const std::string dir = executableDirectory();
    const std::string exePath = dir + "\\VoIPClient.exe";
    if (GetFileAttributesA(exePath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::cerr << "[ERR] VoIPClient.exe not found next to GameClientMock.exe: " << exePath << "\n";
        return false;
    }

    std::string cmdLineText = buildVoipClientCommandLine(exePath, args);
    std::vector<char> cmdLine(cmdLineText.begin(), cmdLineText.end());
    cmdLine.push_back('\0');

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    DWORD creationFlags = args.showVoipConsole ? CREATE_NEW_CONSOLE : CREATE_NO_WINDOW;
    if (!CreateProcessA(
            exePath.c_str(),
            cmdLine.data(),
            nullptr,
            nullptr,
            FALSE,
            creationFlags,
            nullptr,
            dir.c_str(),
            &si,
            &pi)) {
        std::cerr << "[ERR] Failed to launch VoIPClient.exe. GetLastError=" << GetLastError() << "\n";
        return false;
    }

    child.pi = pi;
    child.launched = true;
    std::cout << "[*] Launched VoIPClient.exe"
              << (args.showVoipConsole ? " with console\n" : " without console\n");
    return true;
}

void closeChildHandles(ChildProcess& child) {
    if (!child.launched) return;
    CloseHandle(child.pi.hThread);
    CloseHandle(child.pi.hProcess);
    child.pi = {};
    child.launched = false;
}

bool isChildRunning(const ChildProcess& child) {
    if (!child.launched) return false;
    return WaitForSingleObject(child.pi.hProcess, 0) == WAIT_TIMEOUT;
}

void terminateChildIfRunning(ChildProcess& child) {
    if (!isChildRunning(child)) return;
    TerminateProcess(child.pi.hProcess, 1);
    WaitForSingleObject(child.pi.hProcess, 2000);
}

bool tryConnectExistingIpc(VoIP::IpcClient& ipc, const Args& args) {
    if (args.ipc.transport == VoIP::IpcTransport::NamedPipe &&
        !namedPipeEndpointExists(args.ipc)) {
        return false;
    }

    std::cout << "[*] Checking for existing VoIPClient IPC...\n";
    return ipc.connect(args.ipc, [](const std::string& json) {
        std::cout << "[IPC-EVT] " << json << "\n";
    });
}

bool connectLaunchedIpc(VoIP::IpcClient& ipc, const Args& args, const ChildProcess& child) {
    std::cout << "[*] Waiting for launched VoIPClient IPC...\n";

    const ULONGLONG deadline = GetTickCount64() + 15000;
    int attempt = 0;
    while (g_running && GetTickCount64() < deadline) {
        if (!isChildRunning(child)) {
            DWORD exitCode = 0;
            GetExitCodeProcess(child.pi.hProcess, &exitCode);
            std::cerr << "[ERR] VoIPClient.exe exited before IPC became ready. exitCode="
                      << exitCode << "\n";
            return false;
        }

        ++attempt;
        if (ipc.connect(args.ipc, [](const std::string& json) {
                std::cout << "[IPC-EVT] " << json << "\n";
            })) {
            if (attempt > 1) {
                std::cout << "[*] Connected to launched VoIPClient IPC after retry.\n";
            }
            return true;
        }

        Sleep(args.ipc.transport == VoIP::IpcTransport::NamedPipe ? 250 : 100);
    }

    return false;
}

Args parseArgs(int argc, char* argv[]) {
    Args args;
    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i];

        if (key == "--show-voip-console") {
            args.showVoipConsole = true;
            continue;
        }
        if (key == "--hide-voip-console") {
            args.showVoipConsole = false;
            continue;
        }

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
    std::cout << "  VoIP UI  : "
              << (args.showVoipConsole ? "show console" : "hide console") << "\n";
    std::cout << "========================================\n";

    VoIP::IpcClient ipc;
    ChildProcess voipClient;
    bool connected = tryConnectExistingIpc(ipc, args);
    if (connected) {
        std::cout << "[*] Attached to existing VoIPClient.exe\n";
    } else {
        std::cout << "[*] No existing IPC endpoint. Launching VoIPClient.exe...\n";
        if (!launchVoipClient(args, voipClient)) {
            return 1;
        }
        connected = connectLaunchedIpc(ipc, args, voipClient);
    }

    if (!connected) {
        std::cerr << "[ERR] Could not connect to VoIP Client IPC.\n";
        terminateChildIfRunning(voipClient);
        closeChildHandles(voipClient);
        return 1;
    }

    ipc.send("{\"ver\":1,\"cmd\":\"HELLO\"}");
    ipc.send("{\"ver\":1,\"cmd\":\"STATUS\"}");

    std::thread heartbeatThread([&]() {
        while (g_running.load()) {
            ipc.send("{\"ver\":1,\"cmd\":\"HEARTBEAT\"}");
            std::this_thread::sleep_for(std::chrono::milliseconds(HEARTBEAT_INTERVAL_MS));
        }
    });

    printHelp();
    std::cout << "\n";

    bool quitClientSent = false;
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
            quitClientSent = true;
            continue;
        }
        if (cmd == "quit" || cmd == "exit") {
            g_running = false;
            break;
        }

        std::cout << "Unknown command: " << cmd << "\n";
        printHelp();
    }

    g_running = false;
    if (heartbeatThread.joinable()) {
        heartbeatThread.join();
    }

    if (!quitClientSent) {
        ipc.send("{\"ver\":1,\"cmd\":\"QUIT\"}");
        quitClientSent = true;
    }

    ipc.disconnect();
    if (quitClientSent && voipClient.launched) {
        WaitForSingleObject(voipClient.pi.hProcess, 2000);
    }
    closeChildHandles(voipClient);
    return 0;
}
