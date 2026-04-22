#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "VoIP/VoIP.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
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
    std::cerr << "    Client.exe --ipc-type socket [--ipc-port 17832]\n";
    std::cerr << "    Client.exe --ipc-type namedPipe [--ipc-name RanOnlineVoIP]\n";
    std::cerr << "\n";
    std::cerr << "  Direct startup mode:\n";
    std::cerr << "    Client.exe --server 127.0.0.1:7000 --token test "
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

bool hasAnyJoinArgs(const Args& args) {
    return args.serverProvided || args.tokenProvided ||
           args.channelProvided || args.playerProvided;
}

bool fileExists(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return static_cast<bool>(in);
}

std::string readTextFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

void writeDefaultClientConfigFile(const std::string& path,
                                  const VoIP::ChannelConfig& cfg) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return;

    out << "{\n";
    out << "  \"signalingServer\": \"" << cfg.signalingServer << "\",\n";
    out << "  \"signalingPort\": " << cfg.signalingPort << ",\n";
    out << "  \"token\": \"" << cfg.token << "\",\n";
    out << "  \"channelId\": \"" << cfg.channelId << "\",\n";
    out << "  \"playerId\": \"" << cfg.playerId << "\",\n";
    out << "\n";
    out << "  \"opusBitrate\": " << cfg.opusBitrate << ",\n";
    out << "  \"captureVadDb\": " << cfg.captureVadDb << ",\n";
    out << "  \"captureHangoverFrames\": " << cfg.captureHangoverFrames << ",\n";
    out << "\n";
    out << "  \"enableEchoCancel\": "
        << (cfg.enableEchoCancel ? "true" : "false") << ",\n";
    out << "  \"aecFilterMs\": " << cfg.aecFilterMs << ",\n";
    out << "  \"aecEchoSuppress\": " << cfg.aecEchoSuppress << ",\n";
    out << "  \"aecEchoSuppressActive\": " << cfg.aecEchoSuppressActive << ",\n";
    out << "\n";
    out << "  \"enableDenoise\": "
        << (cfg.enableDenoise ? "true" : "false") << "\n";
    out << "}\n";
}

std::string jStr(const std::string& j, const std::string& key) {
    const std::string pat = "\"" + key + "\"";
    const auto k = j.find(pat);
    if (k == std::string::npos) return {};
    const auto c = j.find(':', k + pat.size());
    if (c == std::string::npos) return {};
    const auto q1 = j.find('"', c + 1);
    if (q1 == std::string::npos) return {};
    const auto q2 = j.find('"', q1 + 1);
    if (q2 == std::string::npos) return {};
    return j.substr(q1 + 1, q2 - q1 - 1);
}

bool jBool(const std::string& j, const std::string& key, bool& out) {
    const std::string pat = "\"" + key + "\"";
    const auto k = j.find(pat);
    if (k == std::string::npos) return false;
    const auto c = j.find(':', k + pat.size());
    if (c == std::string::npos) return false;
    size_t v = c + 1;
    while (v < j.size() &&
           (j[v] == ' ' || j[v] == '\t' || j[v] == '\r' || j[v] == '\n')) {
        ++v;
    }
    if (j.compare(v, 4, "true") == 0) {
        out = true;
        return true;
    }
    if (j.compare(v, 5, "false") == 0) {
        out = false;
        return true;
    }
    return false;
}

bool jInt(const std::string& j, const std::string& key, int& out) {
    const std::string pat = "\"" + key + "\"";
    const auto k = j.find(pat);
    if (k == std::string::npos) return false;
    const auto c = j.find(':', k + pat.size());
    if (c == std::string::npos) return false;
    size_t v = c + 1;
    while (v < j.size() &&
           (j[v] == ' ' || j[v] == '\t' || j[v] == '\r' || j[v] == '\n')) {
        ++v;
    }
    char* end = nullptr;
    const long val = std::strtol(j.c_str() + v, &end, 10);
    if (end == j.c_str() + v) return false;
    out = static_cast<int>(val);
    return true;
}

bool jFloat(const std::string& j, const std::string& key, float& out) {
    const std::string pat = "\"" + key + "\"";
    const auto k = j.find(pat);
    if (k == std::string::npos) return false;
    const auto c = j.find(':', k + pat.size());
    if (c == std::string::npos) return false;
    size_t v = c + 1;
    while (v < j.size() &&
           (j[v] == ' ' || j[v] == '\t' || j[v] == '\r' || j[v] == '\n')) {
        ++v;
    }
    char* end = nullptr;
    const float val = std::strtof(j.c_str() + v, &end);
    if (end == j.c_str() + v) return false;
    out = val;
    return true;
}

void applyClientConfigFile(VoIP::ChannelConfig& cfg, const std::string& path) {
    if (!fileExists(path)) {
        writeDefaultClientConfigFile(path, cfg);
        return;
    }

    const std::string text = readTextFile(path);
    if (text.empty()) return;

    if (const auto v = jStr(text, "signalingServer"); !v.empty()) cfg.signalingServer = v;
    if (int v; jInt(text, "signalingPort", v) && v > 0 && v <= 65535) cfg.signalingPort = static_cast<uint16_t>(v);
    if (const auto v = jStr(text, "turnServer"); !v.empty()) cfg.turnServer = v;
    if (int v; jInt(text, "turnPort", v) && v > 0 && v <= 65535) cfg.turnPort = static_cast<uint16_t>(v);
    if (const auto v = jStr(text, "token"); !v.empty()) cfg.token = v;
    if (const auto v = jStr(text, "channelId"); !v.empty()) cfg.channelId = v;
    if (const auto v = jStr(text, "playerId"); !v.empty()) cfg.playerId = v;

    if (int v; jInt(text, "opusBitrate", v) && v > 0) cfg.opusBitrate = v;
    if (float v; jFloat(text, "captureVadDb", v)) cfg.captureVadDb = v;
    if (int v; jInt(text, "captureHangoverFrames", v) && v >= 0) cfg.captureHangoverFrames = v;
    if (int v; jInt(text, "aecFilterMs", v) && v > 0) cfg.aecFilterMs = v;
    if (int v; jInt(text, "aecEchoSuppress", v)) cfg.aecEchoSuppress = v;
    if (int v; jInt(text, "aecEchoSuppressActive", v)) cfg.aecEchoSuppressActive = v;
    if (bool v; jBool(text, "enableEchoCancel", v)) cfg.enableEchoCancel = v;
    if (bool v; jBool(text, "enableDenoise", v)) cfg.enableDenoise = v;
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

void appendJsonStringField(std::string& json, bool& first,
                           const std::string& key, const std::string& value) {
    if (!first) json += ",";
    first = false;
    json += "\"" + key + "\":\"" + jsonEscape(value) + "\"";
}

void appendJsonBoolField(std::string& json, bool& first,
                         const std::string& key, bool value) {
    if (!first) json += ",";
    first = false;
    json += "\"" + key + "\":" + std::string(value ? "true" : "false");
}

void appendJsonIntField(std::string& json, bool& first,
                        const std::string& key, int value) {
    if (!first) json += ",";
    first = false;
    json += "\"" + key + "\":" + std::to_string(value);
}

bool parseHostPort(const std::string& input,
                   std::string& host,
                   uint16_t& port) {
    const auto colon = input.find(':');
    if (colon == std::string::npos) {
        host = input;
        return !host.empty();
    }

    host = input.substr(0, colon);
    int parsedPort = 0;
    try {
        parsedPort = std::stoi(input.substr(colon + 1));
    } catch (...) {
        return false;
    }
    if (parsedPort <= 0 || parsedPort > 65535) {
        return false;
    }
    port = static_cast<uint16_t>(parsedPort);
    return !host.empty();
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

std::string buildStateJson(const VoIP::ChannelConfig& cfg,
                           bool joined,
                           bool muted,
                           const VoIP::IpcOptions& ipcOptions) {
    std::string json = "{";
    bool first = true;
    appendJsonIntField(json, first, "ver", VoIP::IPC_VERSION);
    appendJsonStringField(json, first, "evt", "STATE");
    appendJsonStringField(json, first, "playerId", cfg.playerId);
    appendJsonStringField(json, first, "channelId", cfg.channelId);
    appendJsonStringField(json, first, "signalingServer", cfg.signalingServer);
    appendJsonIntField(json, first, "signalingPort", cfg.signalingPort);
    appendJsonBoolField(json, first, "joined", joined);
    appendJsonBoolField(json, first, "muted", muted);
    appendJsonStringField(json, first, "ipcType", transportToText(ipcOptions.transport));
    if (ipcOptions.transport == VoIP::IpcTransport::Socket) {
        appendJsonIntField(json, first, "ipcPort", ipcOptions.socketPort);
    } else {
        appendJsonStringField(json, first, "ipcName", ipcOptions.pipeName);
    }
    json += "}";
    return json;
}

std::string buildSimpleEventJson(const std::string& evt) {
    std::string json = "{";
    bool first = true;
    appendJsonIntField(json, first, "ver", VoIP::IPC_VERSION);
    appendJsonStringField(json, first, "evt", evt);
    json += "}";
    return json;
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

    if (ipcMode == false && directMode == false) {
        printClientUsage();
        return 1;
    }

    VoIP::IpcOptions ipcOptions;
    if (ipcMode) {
        if (!parseIpcType(args.ipcType, ipcOptions.transport)) {
            std::cerr << "[WARN] Invalid --ipc-type value: " << args.ipcType << "\n";
            printClientUsage();
            return 1;
        }
        ipcOptions.socketPort = args.ipcPort;
        ipcOptions.pipeName = args.ipcName.empty()
                                ? VoIP::IPC_DEFAULT_PIPE_NAME
                                : args.ipcName;
    }

    std::cout << "========================================\n";
    std::cout << "  VoIP Client  [TEST BUILD]\n";
    std::cout << "========================================\n";
    if (ipcMode) {
        std::cout << "  Mode    : IPC\n";
        std::cout << "  IPC     : " << transportToText(ipcOptions.transport);
        if (ipcOptions.transport == VoIP::IpcTransport::Socket) {
            std::cout << " 127.0.0.1:" << ipcOptions.socketPort << "\n";
        } else {
            std::cout << " \\\\.\\pipe\\" << ipcOptions.pipeName << "\n";
        }
    } else {
        std::cout << "  Mode    : Direct Startup\n";
        std::cout << "  Server  : " << args.serverHost << ":" << args.serverPort << "\n";
        std::cout << "  Channel : " << args.channelId << "\n";
        std::cout << "  Player  : " << args.playerId << "\n";
    }
    std::cout << "========================================\n";
    std::cout << "  q+Enter = quit\n";
    std::cout << "  m+Enter = toggle mute\n";
    std::cout << "========================================\n\n";

    VoIP::Channel channel;
    VoIP::ChannelConfig cfg;
    cfg.signalingServer = args.serverHost;
    cfg.signalingPort = args.serverPort;
    cfg.channelId = args.channelId;
    cfg.playerId = args.playerId;
    cfg.token = args.token;
    applyClientConfigFile(cfg, "client_config.json");

    std::mutex channelMtx;
    std::mutex cfgMtx;
    std::atomic<bool> joined{false};

    VoIP::IpcServer ipc;
    const auto sendIpc = [&](const std::string& json) {
        if (ipcMode) {
            ipc.send(json);
        }
    };

    const auto sendState = [&]() {
        if (!ipcMode) return;
        std::scoped_lock lock(channelMtx, cfgMtx);
        sendIpc(buildStateJson(cfg, joined.load(), channel.isMuted(), ipcOptions));
    };

    VoIP::ChannelEvents ev;
    ev.onPeerJoined = [&](const std::string& id) {
        std::cout << "[+] Peer joined  : " << id << "\n";
        if (!ipcMode) return;
        std::string json = "{";
        bool first = true;
        appendJsonIntField(json, first, "ver", VoIP::IPC_VERSION);
        appendJsonStringField(json, first, "evt", "PEER_JOIN");
        appendJsonStringField(json, first, "playerId", id);
        json += "}";
        sendIpc(json);
    };

    ev.onPeerLeft = [&](const std::string& id) {
        std::cout << "[-] Peer left    : " << id << "\n";
        if (!ipcMode) return;
        std::string json = "{";
        bool first = true;
        appendJsonIntField(json, first, "ver", VoIP::IPC_VERSION);
        appendJsonStringField(json, first, "evt", "PEER_LEAVE");
        appendJsonStringField(json, first, "playerId", id);
        json += "}";
        sendIpc(json);
    };

    ev.onSpeakingChanged = [&](const std::string& id, bool speaking) {
#ifdef _DEBUG
        std::cout << "[~] " << id
                  << (speaking ? " >> SPEAKING" : "    silent") << "\n";
#endif
        if (!ipcMode) return;
        std::string json = "{";
        bool first = true;
        appendJsonIntField(json, first, "ver", VoIP::IPC_VERSION);
        appendJsonStringField(json, first, "evt", "SPEAKING");
        appendJsonStringField(json, first, "playerId", id);
        appendJsonBoolField(json, first, "speaking", speaking);
        json += "}";
        sendIpc(json);
    };

    ev.onError = [&](const std::string& msg) {
        std::cerr << "[ERR] " << msg << "\n";
        if (!ipcMode) return;
        std::string json = "{";
        bool first = true;
        appendJsonIntField(json, first, "ver", VoIP::IPC_VERSION);
        appendJsonStringField(json, first, "evt", "ERROR");
        appendJsonStringField(json, first, "message", msg);
        json += "}";
        sendIpc(json);
    };

    const auto joinCurrentConfig = [&](const std::string& reason) -> bool {
        std::scoped_lock lock(channelMtx, cfgMtx);

        if (joined.load()) {
            channel.leave();
            joined = false;
        }

        std::cout << "[*] Joining channel";
        if (!reason.empty()) {
            std::cout << " (" << reason << ")";
        }
        std::cout << "...\n";

        if (!channel.join(cfg, ev)) {
            std::cerr << "[!] join() failed. Check server is running.\n";
            return false;
        }

        joined = true;
        std::cout << "[*] Joined! Microphone active.\n";
        return true;
    };

    if (ipcMode) {
        if (!ipc.start(ipcOptions, [&](const std::string& jsonMsg) {
                const std::string cmd = jStr(jsonMsg, "cmd");
                if (cmd.empty()) return;

                if (cmd == "HELLO") {
                    sendIpc(buildSimpleEventJson("READY"));
                    sendState();
                    return;
                }

                if (cmd == "STATUS") {
                    sendState();
                    return;
                }

                if (cmd == "LOGIN") {
                    {
                        std::lock_guard<std::mutex> cfgLock(cfgMtx);

                        if (const auto v = jStr(jsonMsg, "server"); !v.empty()) {
                            std::string host = cfg.signalingServer;
                            uint16_t port = cfg.signalingPort;
                            if (parseHostPort(v, host, port)) {
                                cfg.signalingServer = host;
                                cfg.signalingPort = port;
                            }
                        }
                        if (const auto v = jStr(jsonMsg, "signalingServer"); !v.empty()) {
                            cfg.signalingServer = v;
                        }
                        if (int v; jInt(jsonMsg, "signalingPort", v) &&
                                   v > 0 && v <= 65535) {
                            cfg.signalingPort = static_cast<uint16_t>(v);
                        }
                        if (const auto v = jStr(jsonMsg, "token"); !v.empty()) {
                            cfg.token = v;
                        }
                        if (const auto v = jStr(jsonMsg, "playerId"); !v.empty()) {
                            cfg.playerId = v;
                        }
                        if (const auto v = jStr(jsonMsg, "channelId"); !v.empty()) {
                            cfg.channelId = v;
                        }
                    }

                    sendIpc(buildSimpleEventJson("LOGIN_APPLIED"));
                    sendState();
                    return;
                }

                if (cmd == "JOIN") {
                    {
                        std::lock_guard<std::mutex> cfgLock(cfgMtx);
                        if (const auto v = jStr(jsonMsg, "channelId"); !v.empty()) {
                            cfg.channelId = v;
                        }
                        if (const auto v = jStr(jsonMsg, "playerId"); !v.empty()) {
                            cfg.playerId = v;
                        }
                        if (const auto v = jStr(jsonMsg, "token"); !v.empty()) {
                            cfg.token = v;
                        }
                        if (const auto v = jStr(jsonMsg, "server"); !v.empty()) {
                            std::string host = cfg.signalingServer;
                            uint16_t port = cfg.signalingPort;
                            if (parseHostPort(v, host, port)) {
                                cfg.signalingServer = host;
                                cfg.signalingPort = port;
                            }
                        }
                    }

                    if (joinCurrentConfig("IPC")) {
                        sendIpc(buildSimpleEventJson("JOINED"));
                    }
                    sendState();
                    return;
                }

                if (cmd == "LEAVE") {
                    {
                        std::lock_guard<std::mutex> channelLock(channelMtx);
                        if (joined.load()) {
                            channel.leave();
                            joined = false;
                            std::cout << "[*] Left channel from IPC.\n";
                        }
                    }
                    sendIpc(buildSimpleEventJson("LEFT"));
                    sendState();
                    return;
                }

                if (cmd == "MUTE") {
                    bool muted = false;
                    if (!jBool(jsonMsg, "muted", muted)) return;
                    {
                        std::lock_guard<std::mutex> channelLock(channelMtx);
                        channel.setMuted(muted);
                    }
                    std::cout << (muted ? "[MIC] Muted\n" : "[MIC] Unmuted\n");
                    sendIpc(buildSimpleEventJson("MUTE_CHANGED"));
                    sendState();
                    return;
                }

                if (cmd == "TOGGLE_MUTE") {
                    bool muted = false;
                    {
                        std::lock_guard<std::mutex> channelLock(channelMtx);
                        muted = !channel.isMuted();
                        channel.setMuted(muted);
                    }
                    std::cout << (muted ? "[MIC] Muted\n" : "[MIC] Unmuted\n");
                    sendIpc(buildSimpleEventJson("MUTE_CHANGED"));
                    sendState();
                    return;
                }

                if (cmd == "QUIT") {
                    g_running = false;
                }
            })) {
            std::cerr << "[ERR] Failed to start IPC transport.\n";
            printClientUsage();
            return 1;
        }

        std::cout << "[*] Running in IPC mode.\n";
        std::cout << "[*] Waiting for game client commands.\n";
    } else {
        if (!joinCurrentConfig("startup")) {
            return 1;
        }
    }

    std::thread inputThread([&]() {
        std::string line;
        while (g_running && std::getline(std::cin, line)) {
            if (line == "m" || line == "M") {
                bool muted = false;
                {
                    std::lock_guard<std::mutex> channelLock(channelMtx);
                    muted = !channel.isMuted();
                    channel.setMuted(muted);
                }
                std::cout << (muted ? "[MIC] Muted\n" : "[MIC] Unmuted\n");
                if (ipcMode) {
                    sendIpc(buildSimpleEventJson("MUTE_CHANGED"));
                    sendState();
                }
            } else if (line == "q" || line == "Q" || line == "quit") {
                g_running = false;
                break;
            }
        }
        g_running = false;
    });

    auto nextTick = std::chrono::steady_clock::now();
    while (g_running) {
        {
            std::lock_guard<std::mutex> channelLock(channelMtx);
            channel.tick();
        }
        nextTick += std::chrono::milliseconds(20);
        std::this_thread::sleep_until(nextTick);
    }

    std::cout << "\n[*] Leaving channel...\n";
    {
        std::lock_guard<std::mutex> channelLock(channelMtx);
        if (joined.load()) {
            channel.leave();
            joined = false;
        }
    }
    if (ipcMode) {
        ipc.stop();
    }
    std::cout << "[*] Done.\n";

    if (inputThread.joinable()) {
        inputThread.join();
    }

    return 0;
}
