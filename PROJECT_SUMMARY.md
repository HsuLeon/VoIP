# VoIP Project Summary

## Project Goal

This project provides a Windows-native VoIP stack for game voice chat.
It is designed around:

- Low-latency peer voice communication
- Direct P2P audio whenever possible
- Server relay fallback when NAT traversal fails
- Tight integration with a game client/server, but with loose coupling

Current workspace:

- `D:\Projects\VoIP`

## Architecture Overview

The solution is split into four main projects:

- `VoIP`
  Shared static library containing audio I/O, codec, RTP, NAT traversal, jitter, mixing, and channel logic
- `Client`
  Builds `VoIPClient.exe`, the voice client executable used by tests and future game integration
- `Server`
  Builds `VoIPServer.exe`, the signaling server plus TURN-like UDP relay
- `Test`
  Test executable for core logic validation

Third-party libraries currently used:

- `miniaudio`
  Windows audio input/output
- `Opus`
  Voice codec
- `RNNoise`
  Noise suppression
- `SpeexDSP`
  Acoustic echo cancellation and preprocessing support

## Connection Model

The connection flow is:

1. Client opens UDP socket
2. Client queries STUN to learn public endpoint
3. Client connects to signaling server over TCP
4. Client sends `JOIN` with channel/player identity plus UDP endpoint info
5. Server returns current peers and relay port
6. Clients try direct UDP punch-through
7. If direct path is not ready, audio can temporarily use relay
8. When direct path becomes valid, routing switches to direct

Current routing behavior supports:

- Initial relay fallback
- Relay to direct recovery
- Same-NAT LAN preference
- Direct communication continuing even if the signaling server is later stopped

## NAT / Relay / LAN Behavior

The project now handles several important network cases:

- Same machine localhost test
- Two machines on the same LAN
- Two machines using an external server
- Two clients behind the same public IP with LAN candidate preference
- Network change after join, followed by endpoint update and route re-selection

Important ports used by the server:

- `TCP 7000`
  Signaling
- `UDP 40000`
  Relay

## Audio Pipeline

Current client-side capture pipeline:

- Microphone capture
- Speex AEC
- RNNoise denoise
- Voice detection / hangover
- Opus encode
- RTP send

Current playback pipeline:

- Receive RTP
- Jitter buffer
- Opus decode
- Mixer
- Audio playback
- Actual playback PCM is also fed back into AEC as playback reference

## Major Implemented Features

The project has already been strengthened well beyond the initial prototype.

### Build and platform

- `x64` and `x86` configurations added
- `Debug` and `Release` builds verified
- Third-party libraries built for both architectures:
  - `Opus`
  - `RNNoise`
  - `SpeexDSP`

### Audio robustness

- Static/noise playback bug fixed
- Microphone / headphone hot-plug recovery implemented
- Audio device reroute handling added
- Playback queue priming improved
- AEC playback-reference feed aligned to actual playback callback
- Persistent Speex playback/xrun warnings greatly reduced

### Voice quality

- RNNoise integrated before encode
- Speex AEC integrated on client side
- Voice activity gating and hangover tuned for speech use
- Opus bitrate exposed as configuration
- Core audio tuning values now centralized into `ChannelConfig`

### Networking

- STUN query moved onto the actual RTP socket
- Public endpoint signaling implemented
- Direct/relay route debug improved
- Relay-to-direct switching fixed
- Initial speech loss during punch setup reduced by sending via relay while direct path warms up
- LAN candidate reporting added for same-public-IP peers
- Automatic endpoint update added on network change
- Signaling reconnection logic added so clients can re-`JOIN` after TCP loss

## Current State

Based on current testing, the project has successfully demonstrated:

- Localhost voice communication
- Two-machine voice communication
- Direct route establishment
- Relay fallback
- Relay back to direct transition
- LAN direct preference behind the same public IP
- Hot-plug audio device recovery
- Automatic signaling recovery after server restart
- Automatic address update after network change

## Known Design Direction

This system is optimized for player voice chat, not for music fidelity.

That means the current pipeline intentionally favors:

- Speech clarity
- Background noise reduction
- Echo reduction
- Bandwidth efficiency

Because of that, singing/music playback through a microphone may sound thin, choppy, or radio-like. That is expected for a voice-focused configuration.

## Client Configuration

The client now supports `client_config.json`.

Behavior:

- If `client_config.json` exists, the client reads it and overrides defaults
- If it does not exist, the client automatically creates one using the built-in defaults
- Missing fields fall back to defaults

Example format:

```json
{
  "signalingServer": "127.0.0.1",
  "signalingPort": 7000,
  "token": "test",
  "channelId": "guild_123",
  "playerId": "playerA",

  "opusBitrate": 32000,
  "captureVadDb": -50.0,
  "captureHangoverFrames": 12,

  "enableEchoCancel": true,
  "aecFilterMs": 200,
  "aecEchoSuppress": -40,
  "aecEchoSuppressActive": -15,

  "enableDenoise": true
}
```

## IPC and GameClientMock

The solution now also includes:

- `GameClientMock`
  A console project that simulates the future game client and talks to `VoIPClient.exe` through local IPC
- `GameClientMock/src/CVoIP.*`
  A singleton wrapper used by the mock game client to own VoIPClient launch/attach, IPC commands, heartbeat, and shutdown behavior
- `Client/src/CVoIPClientApp.*`
  A singleton wrapper that owns the current VoIP client runtime: channel lifecycle, IPC server, heartbeat timeout, state/event forwarding, config application, mute/join/leave control, and shutdown flow
- `Server/src/CVoIPServer.*`
  A singleton wrapper that owns the current VoIP server runtime: signaling accept loop, UDP relay loop, heartbeat/ping management, session map, and shutdown flow

IPC is currently implemented as a transport-selectable local channel:

- JSON message protocol shared across all IPC modes
- `VoIPClient.exe` acts as the local IPC server
- `GameClientMock.exe` acts as the local IPC client
- Supported IPC transports:
  - `socket`
  - `namedPipe`

### Client startup behavior

`VoIPClient.exe` now supports two startup modes:

- IPC mode:
  Requires `--ipc-type`
- Direct startup mode:
  Requires the full set of voice startup arguments

IPC mode examples:

```powershell
VoIPClient.exe --ipc-type socket --ipc-port 17832
VoIPClient.exe --ipc-type namedPipe --ipc-name RanOnlineVoIP
```

Direct startup example:

```powershell
VoIPClient.exe --server 127.0.0.1:7000 --token test --channel guild_123 --player playerA
```

If the required arguments are incomplete or missing, the client prints a correct usage example and exits.

### GameClientMock usage

Recommended test flow:

1. Start `GameClientMock.exe` with an IPC transport
2. `GameClientMock.exe` first tries to attach to an existing `VoIPClient.exe` on the same IPC endpoint
3. If no compatible IPC endpoint is found, `GameClientMock.exe` launches `VoIPClient.exe` from the same output folder and passes through the IPC arguments
4. Type commands into the `GameClientMock.exe` console and press Enter

Example:

```powershell
GameClientMock.exe --ipc-type socket --ipc-port 17832
```

Or:

```powershell
GameClientMock.exe --ipc-type namedPipe --ipc-name RanOnlineVoIP
```

For production-like testing where the voice client should run in the background:

```powershell
GameClientMock.exe --ipc-type namedPipe --ipc-name RanOnlineVoIP --hide-voip-console
```

Note:

- `VoIPClient.exe` must be in the same folder as `GameClientMock.exe`
- `GameClientMock.exe` shows the launched `VoIPClient.exe` console by default for development
- Add `--hide-voip-console` when the launched voice client should run without a visible terminal window
- When `GameClientMock.exe` exits normally, it asks `VoIPClient.exe` to quit, whether it launched or attached to that voice client
- `GameClientMock.exe` sends an IPC `HEARTBEAT` every second
- In IPC mode, `VoIPClient.exe` automatically exits if it does not receive a heartbeat for about 10 seconds
- In IPC mode, shutdown first attempts a graceful channel/audio/network cleanup; if cleanup blocks for several seconds, the background voice client forces its own process exit
- The public IPC name is the base name such as `RanOnlineVoIP`
- Internally, the implementation uses separate one-way named pipes for stability, but that detail is hidden from the caller

At startup, `GameClientMock.exe` attaches to or starts the voice client, connects to the local IPC channel, and requests current state automatically.

Useful commands:

```txt
help
status
hello
login <playerId> <token> [server host:port] [channelId]
join [channelId]
leave
mute on
mute off
toggle-mute
quit-client
quit
```

Typical manual IPC test:

```txt
status
login playerA test 127.0.0.1:7000 guild_123
join
status
mute on
mute off
leave
```

Important notes:

- `quit`
  Closes `GameClientMock.exe` only
- `quit-client`
  Sends an IPC command that asks `VoIPClient.exe` to terminate

### Current IPC command/event direction

Game-side commands currently supported:

- `HELLO`
- `HEARTBEAT`
- `STATUS`
- `LOGIN`
- `JOIN`
- `LEAVE`
- `MUTE`
- `TOGGLE_MUTE`
- `QUIT`

VoIP-side events currently emitted:

- `READY`
- `STATE`
- `PEER_JOIN`
- `PEER_LEAVE`
- `SPEAKING`
- `ERROR`
- `LOGIN_APPLIED`
- `JOINED`
- `LEFT`
- `MUTE_CHANGED`

## Recommended Deployment Notes

- Open `TCP 7000` and `UDP 40000` on the VoIP server
- If an external server uses source-IP allowlists, mobile or home networks may be blocked even though web browsing still works
- Direct communication may continue even if the signaling server is temporarily unavailable
- External server restrictions should be distinguished from client-side bugs during testing

## Suggested Next Improvements

The project is already highly functional. The most natural future improvements would be:

- Better multi-user room validation and tuning
- Explicit audio device selection
- More polished production/background client mode
- Clearer runtime state reporting for direct/relay/AEC/reconnect
- Additional long-duration stability testing

## Notes on Replaced Documents

This file is intended to replace the older high-level progress/design notes:

- `PROGRESS.md`
- `VoIP_設計重點整理.pdf`

Those files can be removed after you confirm this summary covers the information you want to keep.
