# VoIP 專案開發進度

## 專案路徑
`D:\Projects\VoIP`

## 目前狀態
- [x] VS2022 方案結構建立完成
- [x] 四個專案骨架建立（VoIP.lib / Client / Server / Test）
- [x] 第三方函式庫目錄結構建立
- [x] 下載並編譯 libopus 1.6.1（third_party/opus/）Debug 2.8MB / Release 1.1MB
- [x] 下載並放入 miniaudio.h v0.11.25（third_party/miniaudio/）
- [x] 下載並編譯 RNNoise（third_party/rnnoise/）Debug 236KB / Release 190KB
- [x] 下載並編譯 SpeexDSP（third_party/speexdsp/）Debug 362KB / Release 270KB

## 完成項目（全部）

1. **[x] 實作 Audio.cpp**（miniaudio）✅
   - init()：播放裝置；startCapture()：收音裝置
   - play()：PCM 推入 deque 佇列（mutex 保護，max 500ms）
   - WIN32_LEAN_AND_MEAN + NOMINMAX + pImpl callback 防護

2. **[x] 實作 Network.cpp**（STUN/UDP/RTP）✅
   - queryStun()：RFC 5389 XOR-MAPPED-ADDRESS 解析
   - UdpSocket：bind/send/startRecv（背景 recvfrom 執行緒）
   - RtpPacket：serialize/parse；JitterBuffer：序號排序最小堆

3. **[x] 實作 Ipc.cpp**（TCP 127.0.0.1:17832）✅
   - IpcServer / IpcClient，換行分隔 JSON，partial recv 緩衝

4. **[x] Phase 1 單元測試通過** ✅
   - Opus roundtrip（22 bytes）、Mixer（3000）、VAD（silence/loud）

5. **[x] 實作 Channel.cpp**（語音頻道完整管線）✅
   - PeerConn：ssrc/punchDone/useTurn/decoder/jitter/VAD 防抖
   - join()：9 步初始化（Winsock→音訊→UDP→TCP→JOIN→JOINED→startRecv→sigThread→startCapture）
   - tick()：打洞計時 → jitter 解碼 → VAD 防抖 → mixer → play
   - leave()：完整清理序列
   - TURN relay：sentViaRelay 旗標防重複發送

6. **[x] 實作 Server/main.cpp**（Signaling + TURN Relay）✅
   - SignalingServer TCP:7000：JOIN/JOINED/PEER_JOIN/PEER_LEAVE/PING/PONG
   - TURN Relay UDP:40000：SSRC 辨識發送者，廣播給同頻道 peers
   - 學習外部 UDP 地址（首次 relay 封包自動填入 udpExtIp/udpExtPort）
   - Heartbeat：30s PING / 60s PONG 超時踢除
   - Ctrl+C 優雅關機（SetConsoleCtrlHandler）

## 下一步（待辦）

### 🔴 明天繼續：端對端語音測試
**Server.exe 已成功啟動**，輸出如下（確認正常）：
```
========================================
  VoIP Server
  Signaling : TCP:7000
  TURN Relay: UDP:40000
========================================
[TURN] Listening on UDP:40000
[SIG] Listening on TCP:7000
```

**Client/main.cpp 已改為測試版（TEST BUILD）**：
- 跳過 IPC，直接從命令列參數自動 join
- 支援 Enter 離開、m+Enter 切換靜音
- onPeerJoined / onPeerLeft / onSpeakingChanged 事件會印到 console

**測試步驟（明天）：**
1. 編譯 Client 專案（VS2022 → Build Client）
2. 開三個 cmd 視窗：
   - cmd 1：保持 Server.exe 執行
   - cmd 2：`VoIPClient.exe --server 127.0.0.1:7000 --token test --channel guild_123 --player playerA`
   - cmd 3：`VoIPClient.exe --server 127.0.0.1:7000 --token test --channel guild_123 --player playerB`
3. 預期 Server 印出 JOIN/JOINED，Client 印出 `[+] Peer joined`
4. 對麥克風說話，另一端應聽到聲音並印出 `[~] playerX >> SPEAKING`

**若測試失敗的 debug 方向：**
- onError 印出錯誤訊息 → 確認 Signaling 連線
- 有 JOINED 但沒聲音 → 可能是 VAD 閾值太高或麥克風未初始化
- 有聲音但延遲大 → jitter buffer 大小（預設 12 幀 = 240ms）

### 後續待辦
- [ ] **Client/main.cpp 正式版**：實作 IPC JSON 解析（JOIN/LEAVE/MUTE 指令）
- [ ] **Token 驗證**：Server 呼叫亂2Online REST API 驗證玩家 token
- [ ] Windows Server 2019 防火牆：開放 TCP 7000 + UDP 40000
- [ ] WAN 測試（真實 NAT 環境，驗證打洞與 TURN 切換）

## 備注
- 所有專案統一使用 x64，Debug/Release 使用 MD（動態 CRT）
- 輸出目錄統一為 build/x64/Debug/ 與 build/x64/Release/
- TURN Server Port 範圍規劃：UDP 40000~41000
- IPC 協定版本號：目前為 v1（Ipc.h 的 IPC_VERSION）

## 函式庫設定（PDF 第 9 章提醒）
- [x] 統一 x64，移除 x86 設定
- [ ] 確認 Debug 版第三方函式庫版本
- [ ] Windows Server 2019 防火牆開放 UDP 40000~41000 與 TCP 7000
