#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "CVoIP.h"

#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int HEARTBEAT_INTERVAL_MS = 1000;

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

std::string buildVoipClientCommandLine(const std::string& exePath,
                                       const CVoIPOptions& options) {
    std::string cmd = quoteArg(exePath);
    cmd += " --ipc-type ";
    cmd += transportToText(options.ipc.transport);
    if (options.ipc.transport == VoIP::IpcTransport::Socket) {
        cmd += " --ipc-port ";
        cmd += std::to_string(options.ipc.socketPort);
    } else {
        cmd += " --ipc-name ";
        cmd += quoteArg(options.ipc.pipeName);
    }
    return cmd;
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

HANDLE asHandle(void* handle) {
    return static_cast<HANDLE>(handle);
}

} // namespace

CVoIP::CVoIP() = default;

CVoIP::~CVoIP() {
    stop();
}

bool CVoIP::start(const CVoIPOptions& options) {
    stop();
    m_options = options;
    m_quitSent = false;

    bool connected = tryConnectExistingIpc();
    if (connected) {
        std::cout << "[*] Attached to existing VoIPClient.exe\n";
    } else {
        std::cout << "[*] No existing IPC endpoint. Launching VoIPClient.exe...\n";
        if (!launchVoipClient()) {
            return false;
        }
        connected = connectLaunchedIpc();
    }

    if (!connected) {
        std::cerr << "[ERR] Could not connect to VoIP Client IPC.\n";
        terminateChildIfRunning();
        closeChildHandles();
        return false;
    }

    m_running = true;
    hello();
    startHeartbeat();
    return true;
}

void CVoIP::stop() {
    stopHeartbeat();

    if (!m_quitSent.load()) {
        quitClient();
    }

    m_ipc.disconnect();

    if (m_quitSent.load() && m_child.launched) {
        WaitForSingleObject(asHandle(m_child.process), 2000);
    }
    closeChildHandles();
}

void CVoIP::hello() {
    sendRaw("{\"ver\":1,\"cmd\":\"HELLO\"}");
}

void CVoIP::status() {
    sendRaw("{\"ver\":1,\"cmd\":\"STATUS\"}");
}

void CVoIP::login(const std::string& playerId,
                  const std::string& token,
                  const std::string& server,
                  const std::string& channelId) {
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
    sendRaw(json);
}

void CVoIP::join(const std::string& channelId) {
    std::string json = "{\"ver\":1,\"cmd\":\"JOIN\"";
    if (!channelId.empty()) {
        json += ",\"channelId\":\"" + jsonEscape(channelId) + "\"";
    }
    json += "}";
    sendRaw(json);
}

void CVoIP::leave() {
    sendRaw("{\"ver\":1,\"cmd\":\"LEAVE\"}");
}

void CVoIP::setMute(bool muted) {
    sendRaw(std::string("{\"ver\":1,\"cmd\":\"MUTE\",\"muted\":") +
            (muted ? "true}" : "false}"));
}

void CVoIP::toggleMute() {
    sendRaw("{\"ver\":1,\"cmd\":\"TOGGLE_MUTE\"}");
}

void CVoIP::quitClient() {
    sendRaw("{\"ver\":1,\"cmd\":\"QUIT\"}");
    m_quitSent = true;
}

bool CVoIP::tryConnectExistingIpc() {
    if (m_options.ipc.transport == VoIP::IpcTransport::NamedPipe &&
        !namedPipeEndpointExists(m_options.ipc)) {
        return false;
    }

    std::cout << "[*] Checking for existing VoIPClient IPC...\n";
    return m_ipc.connect(m_options.ipc, [](const std::string& json) {
        std::cout << "[IPC-EVT] " << json << "\n";
    });
}

bool CVoIP::launchVoipClient() {
    const std::string dir = executableDirectory();
    const std::string exePath = dir + "\\VoIPClient.exe";
    if (GetFileAttributesA(exePath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::cerr << "[ERR] VoIPClient.exe not found next to GameClientMock.exe: "
                  << exePath << "\n";
        return false;
    }

    std::string cmdLineText = buildVoipClientCommandLine(exePath, m_options);
    std::vector<char> cmdLine(cmdLineText.begin(), cmdLineText.end());
    cmdLine.push_back('\0');

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    const DWORD creationFlags =
        m_options.showVoipConsole ? CREATE_NEW_CONSOLE : CREATE_NO_WINDOW;
    if (!CreateProcessA(exePath.c_str(),
                        cmdLine.data(),
                        nullptr,
                        nullptr,
                        FALSE,
                        creationFlags,
                        nullptr,
                        dir.c_str(),
                        &si,
                        &pi)) {
        std::cerr << "[ERR] Failed to launch VoIPClient.exe. GetLastError="
                  << GetLastError() << "\n";
        return false;
    }

    m_child.process = pi.hProcess;
    m_child.thread = pi.hThread;
    m_child.launched = true;
    std::cout << "[*] Launched VoIPClient.exe"
              << (m_options.showVoipConsole ? " with console\n" : " without console\n");
    return true;
}

bool CVoIP::connectLaunchedIpc() {
    std::cout << "[*] Waiting for launched VoIPClient IPC...\n";

    const ULONGLONG deadline = GetTickCount64() + 15000;
    int attempt = 0;
    while (GetTickCount64() < deadline) {
        if (!isChildRunning()) {
            DWORD exitCode = 0;
            GetExitCodeProcess(asHandle(m_child.process), &exitCode);
            std::cerr << "[ERR] VoIPClient.exe exited before IPC became ready. exitCode="
                      << exitCode << "\n";
            return false;
        }

        ++attempt;
        if (m_ipc.connect(m_options.ipc, [](const std::string& json) {
                std::cout << "[IPC-EVT] " << json << "\n";
            })) {
            if (attempt > 1) {
                std::cout << "[*] Connected to launched VoIPClient IPC after retry.\n";
            }
            return true;
        }

        Sleep(m_options.ipc.transport == VoIP::IpcTransport::NamedPipe ? 250 : 100);
    }

    return false;
}

bool CVoIP::isChildRunning() const {
    if (!m_child.launched) return false;
    return WaitForSingleObject(asHandle(m_child.process), 0) == WAIT_TIMEOUT;
}

void CVoIP::terminateChildIfRunning() {
    if (!isChildRunning()) return;
    TerminateProcess(asHandle(m_child.process), 1);
    WaitForSingleObject(asHandle(m_child.process), 2000);
}

void CVoIP::closeChildHandles() {
    if (!m_child.launched) return;
    CloseHandle(asHandle(m_child.thread));
    CloseHandle(asHandle(m_child.process));
    m_child = {};
}

void CVoIP::startHeartbeat() {
    stopHeartbeat();
    m_running = true;
    m_heartbeatThread = std::thread([this]() {
        while (m_running.load()) {
            sendRaw("{\"ver\":1,\"cmd\":\"HEARTBEAT\"}");
            std::this_thread::sleep_for(std::chrono::milliseconds(HEARTBEAT_INTERVAL_MS));
        }
    });
}

void CVoIP::stopHeartbeat() {
    m_running = false;
    if (m_heartbeatThread.joinable()) {
        m_heartbeatThread.join();
    }
}

void CVoIP::sendRaw(const std::string& json) {
    m_ipc.send(json);
}
