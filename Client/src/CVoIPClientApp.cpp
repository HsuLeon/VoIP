#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "CVoIPClientApp.h"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <thread>

namespace {

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
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
    appendJsonStringField(json, first, "ipcType",
                          CVoIPClientApp::transportToText(ipcOptions.transport));
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

CVoIPClientApp::CVoIPClientApp() = default;

CVoIPClientApp::~CVoIPClientApp() {
    stop();
}

bool CVoIPClientApp::parseIpcType(const std::string& text,
                                  VoIP::IpcTransport& out) {
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

std::string CVoIPClientApp::transportToText(VoIP::IpcTransport transport) {
    return transport == VoIP::IpcTransport::Socket ? "socket" : "namedPipe";
}

void CVoIPClientApp::applyClientConfigFile(VoIP::ChannelConfig& cfg,
                                           const std::string& path) {
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

bool CVoIPClientApp::start(const CVoIPClientAppOptions& options) {
    stop();

    m_options = options;
    m_cfg = options.initialConfig;
    m_running = true;
    m_joined = false;
    m_lastIpcHeartbeatMs = nowMs();
    m_started = true;

    setupChannelEvents();

    if (m_options.ipcMode) {
        if (!m_ipc.start(m_options.ipcOptions, [&](const std::string& jsonMsg) {
                handleIpcMessage(jsonMsg);
            })) {
            std::cerr << "[ERR] Failed to start IPC transport.\n";
            m_running = false;
            m_started = false;
            return false;
        }

        std::cout << "[*] Running in IPC mode.\n";
        std::cout << "[*] Waiting for game client commands.\n";
    }

    if (m_options.autoJoinOnStart && !joinCurrentConfig("startup")) {
        if (m_options.ipcMode) {
            m_ipc.stop();
        }
        m_running = false;
        m_started = false;
        return false;
    }

    return true;
}

void CVoIPClientApp::stop() {
    if (!m_started) return;

    m_running = false;

    const auto shutdownWork = [&]() {
        std::cout << "\n[*] Leaving channel...\n";
        {
            std::lock_guard<std::mutex> channelLock(m_channelMtx);
            if (m_joined.load()) {
                m_channel.leave();
                m_joined = false;
            }
        }
        if (m_options.ipcMode) {
            m_ipc.stop();
        }
        std::cout << "[*] Done.\n";
    };

    if (m_options.ipcMode) {
        std::atomic<bool> shutdownDone{false};
        std::thread shutdownThread([&]() {
            shutdownWork();
            shutdownDone = true;
        });

        const auto deadline =
            std::chrono::steady_clock::now() +
            std::chrono::milliseconds(m_options.ipcShutdownGraceMs);
        while (!shutdownDone.load() &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        if (!shutdownDone.load() && m_options.forceExitOnIpcShutdownTimeout) {
            std::cerr << "[ERR] IPC shutdown grace period exceeded. Forcing VoIP client exit.\n";
            ExitProcess(0);
        }

        if (shutdownThread.joinable()) {
            shutdownThread.join();
        }
    } else {
        shutdownWork();
    }

    m_started = false;
}

void CVoIPClientApp::tick() {
    if (!m_running.load()) return;

    if (m_options.ipcMode &&
        nowMs() - m_lastIpcHeartbeatMs.load() > m_options.ipcHeartbeatTimeoutMs) {
        std::cerr << "[ERR] IPC heartbeat timeout. Closing VoIP client.\n";
        m_running = false;
        return;
    }

    std::lock_guard<std::mutex> channelLock(m_channelMtx);
    m_channel.tick();
}

void CVoIPClientApp::requestQuit() {
    m_running = false;
}

bool CVoIPClientApp::isRunning() const {
    return m_running.load();
}

bool CVoIPClientApp::isIpcMode() const {
    return m_options.ipcMode;
}

bool CVoIPClientApp::isJoined() const {
    return m_joined.load();
}

bool CVoIPClientApp::isMuted() const {
    std::lock_guard<std::mutex> channelLock(m_channelMtx);
    return m_channel.isMuted();
}

bool CVoIPClientApp::joinCurrentConfig(const std::string& reason) {
    std::scoped_lock lock(m_channelMtx, m_cfgMtx);

    if (m_joined.load()) {
        m_channel.leave();
        m_joined = false;
    }

    std::cout << "[*] Joining channel";
    if (!reason.empty()) {
        std::cout << " (" << reason << ")";
    }
    std::cout << "...\n";

    if (!m_channel.join(m_cfg, m_events)) {
        std::cerr << "[!] join() failed. Check server is running.\n";
        return false;
    }

    m_joined = true;
    std::cout << "[*] Joined! Microphone active.\n";
    return true;
}

void CVoIPClientApp::leaveChannel(const std::string& reason) {
    std::lock_guard<std::mutex> channelLock(m_channelMtx);
    if (!m_joined.load()) return;

    m_channel.leave();
    m_joined = false;

    std::cout << "[*] Left channel";
    if (!reason.empty()) {
        std::cout << " (" << reason << ")";
    }
    std::cout << ".\n";
}

void CVoIPClientApp::setMuted(bool muted) {
    {
        std::lock_guard<std::mutex> channelLock(m_channelMtx);
        m_channel.setMuted(muted);
    }
    std::cout << (muted ? "[MIC] Muted\n" : "[MIC] Unmuted\n");
}

void CVoIPClientApp::toggleMuted() {
    bool muted = false;
    {
        std::lock_guard<std::mutex> channelLock(m_channelMtx);
        muted = !m_channel.isMuted();
        m_channel.setMuted(muted);
    }
    std::cout << (muted ? "[MIC] Muted\n" : "[MIC] Unmuted\n");
}

void CVoIPClientApp::applyLogin(const std::string& playerId,
                                const std::string& token,
                                const std::string& server,
                                const std::string& channelId) {
    std::lock_guard<std::mutex> cfgLock(m_cfgMtx);

    if (!server.empty()) {
        std::string host = m_cfg.signalingServer;
        uint16_t port = m_cfg.signalingPort;
        if (parseHostPort(server, host, port)) {
            m_cfg.signalingServer = host;
            m_cfg.signalingPort = port;
        }
    }
    if (!token.empty()) {
        m_cfg.token = token;
    }
    if (!playerId.empty()) {
        m_cfg.playerId = playerId;
    }
    if (!channelId.empty()) {
        m_cfg.channelId = channelId;
    }
}

VoIP::ChannelConfig CVoIPClientApp::snapshotConfig() const {
    std::lock_guard<std::mutex> cfgLock(m_cfgMtx);
    return m_cfg;
}

void CVoIPClientApp::setupChannelEvents() {
    m_events.onPeerJoined = [&](const std::string& id) {
        std::cout << "[+] Peer joined  : " << id << "\n";
        if (!m_options.ipcMode) return;
        std::string json = "{";
        bool first = true;
        appendJsonIntField(json, first, "ver", VoIP::IPC_VERSION);
        appendJsonStringField(json, first, "evt", "PEER_JOIN");
        appendJsonStringField(json, first, "playerId", id);
        json += "}";
        sendIpc(json);
    };

    m_events.onPeerLeft = [&](const std::string& id) {
        std::cout << "[-] Peer left    : " << id << "\n";
        if (!m_options.ipcMode) return;
        std::string json = "{";
        bool first = true;
        appendJsonIntField(json, first, "ver", VoIP::IPC_VERSION);
        appendJsonStringField(json, first, "evt", "PEER_LEAVE");
        appendJsonStringField(json, first, "playerId", id);
        json += "}";
        sendIpc(json);
    };

    m_events.onSpeakingChanged = [&](const std::string& id, bool speaking) {
        if (m_options.printSpeakingDebug) {
            std::cout << "[~] " << id
                      << (speaking ? " >> SPEAKING" : "    silent") << "\n";
        }
        if (!m_options.ipcMode) return;
        std::string json = "{";
        bool first = true;
        appendJsonIntField(json, first, "ver", VoIP::IPC_VERSION);
        appendJsonStringField(json, first, "evt", "SPEAKING");
        appendJsonStringField(json, first, "playerId", id);
        appendJsonBoolField(json, first, "speaking", speaking);
        json += "}";
        sendIpc(json);
    };

    m_events.onError = [&](const std::string& msg) {
        std::cerr << "[ERR] " << msg << "\n";
        if (!m_options.ipcMode) return;
        sendError(msg);
    };
}

void CVoIPClientApp::handleIpcMessage(const std::string& jsonMsg) {
    const std::string cmd = jStr(jsonMsg, "cmd");
    if (cmd.empty()) return;

    if (cmd == "HEARTBEAT") {
        m_lastIpcHeartbeatMs = nowMs();
        return;
    }

    if (cmd == "HELLO") {
        m_lastIpcHeartbeatMs = nowMs();
        sendSimpleEvent("READY");
        sendState();
        return;
    }

    if (cmd == "STATUS") {
        sendState();
        return;
    }

    if (cmd == "LOGIN") {
        applyLogin(jStr(jsonMsg, "playerId"),
                   jStr(jsonMsg, "token"),
                   jStr(jsonMsg, "server"),
                   jStr(jsonMsg, "channelId"));

        {
            std::lock_guard<std::mutex> cfgLock(m_cfgMtx);
            if (const auto v = jStr(jsonMsg, "signalingServer"); !v.empty()) {
                m_cfg.signalingServer = v;
            }
            if (int v; jInt(jsonMsg, "signalingPort", v) &&
                       v > 0 && v <= 65535) {
                m_cfg.signalingPort = static_cast<uint16_t>(v);
            }
        }

        sendSimpleEvent("LOGIN_APPLIED");
        sendState();
        return;
    }

    if (cmd == "JOIN") {
        applyLogin(jStr(jsonMsg, "playerId"),
                   jStr(jsonMsg, "token"),
                   jStr(jsonMsg, "server"),
                   jStr(jsonMsg, "channelId"));

        if (joinCurrentConfig("IPC")) {
            sendSimpleEvent("JOINED");
        }
        sendState();
        return;
    }

    if (cmd == "LEAVE") {
        leaveChannel("IPC");
        sendSimpleEvent("LEFT");
        sendState();
        return;
    }

    if (cmd == "MUTE") {
        bool muted = false;
        if (!jBool(jsonMsg, "muted", muted)) return;
        setMuted(muted);
        sendSimpleEvent("MUTE_CHANGED");
        sendState();
        return;
    }

    if (cmd == "TOGGLE_MUTE") {
        toggleMuted();
        sendSimpleEvent("MUTE_CHANGED");
        sendState();
        return;
    }

    if (cmd == "QUIT") {
        requestQuit();
    }
}

void CVoIPClientApp::sendIpc(const std::string& jsonMsg) {
    if (m_options.ipcMode) {
        m_ipc.send(jsonMsg);
    }
}

void CVoIPClientApp::sendState() {
    if (!m_options.ipcMode) return;
    std::scoped_lock lock(m_channelMtx, m_cfgMtx);
    sendIpc(buildStateJson(m_cfg,
                           m_joined.load(),
                           m_channel.isMuted(),
                           m_options.ipcOptions));
}

void CVoIPClientApp::sendSimpleEvent(const std::string& evt) {
    sendIpc(buildSimpleEventJson(evt));
}

void CVoIPClientApp::sendError(const std::string& msg) {
    std::string json = "{";
    bool first = true;
    appendJsonIntField(json, first, "ver", VoIP::IPC_VERSION);
    appendJsonStringField(json, first, "evt", "ERROR");
    appendJsonStringField(json, first, "message", msg);
    json += "}";
    sendIpc(json);
}
