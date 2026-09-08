# Architecture & Troubleshooting Guide

This is a fork of [stedrow/birdnetgo-m5stack-atom-echo-rtsp-mic](https://github.com/stedrow/birdnetgo-m5stack-atom-echo-rtsp-mic). The Atom Echo port, PDM microphone support, Web UI and thermal protection described below come from that project; v3.0.0 rewrote the streaming pipeline on top of it.

## Hardware

- **Board**: M5Stack Atom Echo (ESP32-PICO-D4)
- **Microphone**: SPM1423 PDM MEMS (built-in)
- **Audio**: 16-bit PCM, 16kHz mono (configurable 8–48kHz)
- **Streaming**: RTSP/RTP over TCP, port 8554

### Pin Configuration
```cpp
I2S_BCLK_PIN    = 19  // Bit Clock
I2S_LRCLK_PIN   = 33  // Left-Right Clock / Word Select
I2S_DATA_IN_PIN = 23  // Microphone Data Input (PDM)
I2S_DATA_OUT_PIN = 22 // Speaker Data Output (not used)
```

## Architecture

### Pipeline (v3.0.0)

Three actors, two FreeRTOS queues:

- **Capture task** (core 1, priority 20, runs from boot): `i2s_read` → HPF → ramped gain/AGC → complete RTP frame (`$` interleave header + RTP header + big-endian L16) written into a pre-allocated `TxFrame` slot → pointer pushed to `readyQ`. It always meters the signal (Web UI level works without a client) and is the only LED writer.
- **Sender task** (core 0, priority 8, created per RTSP session): pops frames from `readyQ`, sends them on the raw lwIP socket with `send(MSG_DONTWAIT)` + `select()`, returns slots to `freeQ`. It always finishes a frame it has started, so the interleaved framing cannot desynchronise. It answers in-stream RTSP requests (OPTIONS / GET_PARAMETER keepalive / TEARDOWN) at frame boundaries and skips interleaved RTCP. If the socket accepts no bytes for 10 s the client is considered dead.
- **Arduino `loop()`** (core 1, priority 1 — `CONFIG_ARDUINO_RUNNING_CORE=1`, so it shares core 1 with capture): RTSP negotiation, Web UI, diagnostics. It is the only user of the `WiFiClient` object and closes it only after the sender has confirmed exit through a semaphore.

Buffering: the I2S DMA ring is 8 × 512 frames, of which 7 (224 ms at 16 kHz) can wait for the capture task; the frame pool holds ~1.5 s (24 × 2064 B at the default buffer size, capped at 64 KB and at 80 KB below the largest free heap block). When the pool is exhausted the capture task recycles the oldest queued frame and counts a drop; RTP sequence/timestamp still advance, so the receiver sees a real gap rather than a time-compressed splice.

Why the old design clicked: capture and TCP send ran in one task with a 22.5 ms DMA ring, and `WiFiClient::write()` blocks up to 1 s per retry (10 retries) once lwIP's 5760-byte send buffer is full. Every WiFi hiccup longer than ~150 ms lost samples, and a frame abandoned after a partial write corrupted the `$`-framing for the receiver.

### Cross-Core Safety
- FreeRTOS queues carry frame pointers between cores; frames are allocated once.
- `portMUX_TYPE` spinlocks guard the fixed-size log ring and the HPF coefficient hand-off (no heap inside critical sections).
- Xtensa `memw` barriers on `isStreaming`, `stopStreamRequested`, `capturePauseRequested`.
- Sample-rate / buffer-size changes park the capture task (`pauseCapture`), reinstall I2S, reallocate the pool and resume; the session ends and the client reconnects. Gain, HPF and AGC changes apply live.

### Power and Heat
Controllable consumers, largest first: WiFi receiver always on with power save OFF (~80–100 mA), CPU 160 vs 80 MHz (~10 mA), a spinning `loop()` (now yields), TX power (a few mA at this duty cycle), LED (<2 mA at the library's brightness). The internal temperature sensor is uncalibrated and reads high; thermal protection therefore requires three consecutive readings over the limit.

## Audio Tuning

### Signal Levels
- **Target**: 30–70% (about -10 to -3 dBFS)
- **LED green**: Good level
- **LED orange**: Getting hot — consider reducing gain
- **LED red**: Clipping — reduce gain immediately
- **LED dim purple**: Very quiet — increase gain or enable AGC

### AGC vs Manual Gain
- **AGC OFF**: Consistent, predictable levels (e.g., close-range recording)
- **AGC ON**: Outdoor BirdNET-Go deployment where bird distance varies. AGC multiplies on top of your manual gain setting.

### Buffer Size (samples per RTP packet)

Since 3.0.0 stability no longer depends on this setting: the send queue always holds about 1.5 s of audio. Buffer size only trades packet rate (WiFi airtime, ACK traffic) against per-packet latency.

| Size | Packet | Packets/s at 16 kHz | Notes |
|------|--------|---------------------|-------|
| 256 | 16 ms | 62 | Lowest latency, most WiFi frames |
| 512 | 32 ms | 31 | |
| **1024** | **64 ms** | **16** | **Recommended for BirdNET-Go** |
| 2048+ | 128 ms+ | 8 or fewer | Fewest packets; each packet spans several TCP segments |

## Web UI Features

- IP address and WiFi signal strength
- WiFi TX power control (-1.0 to 19.5 dBm) and WiFi power save toggle
- Free heap memory and system uptime
- RTSP connection status, packet rate and Stream Health (queue depth, drops, I2S overruns, longest stall)
- Real-time signal level and clipping detection
- Audio settings (sample rate, gain, buffer, HPF, AGC)
- CPU frequency selection (80, 160, 240 MHz)
- Thermal protection config (30–95°C limit)
- Auto recovery and scheduled resets
- Timestamped log viewer with copy button

## Troubleshooting

### LED is Yellow (Stuck in Startup)
- Check Serial Monitor for errors
- WiFi credentials may be incorrect
- Reset WiFi: connect to `ESP32-RTSP-Mic-AP` and reconfigure

### LED is Red (Not Streaming)
- Thermal protection triggered
- Allow to cool down, then clear latch via Web UI
- Consider lowering CPU frequency or improving ventilation

### No Audio / Low Volume
1. Check signal level in Web UI
2. Enable AGC (auto-adjusts gain)
3. Increase gain (try 5.0–10.0x)
4. Disable high-pass filter temporarily to test

### Audio Clipping / Distortion
1. Decrease gain
2. Enable AGC (fast attack prevents clipping)
3. Check signal level — aim for 30–70%

### Stream Drops / Clicks
1. Read **Stream Health** in the Web UI: dropped frames > 0 means the client or WiFi could not take 1.5 s of audio; I2S overruns > 0 means the capture task was starved (should never happen).
2. Check WiFi signal strength (RSSI > -70 dBm)
3. If WiFi Power Save is ON and drops appear, the firmware suspends it until reboot after 10 dropped frames and says so in the log; your AP's DTIM period is too long for the 5760-byte TCP send window
4. Serial prints `[Capture] ...` statistics every 30 s while streaming and `[Sender] session ended: <reason>` on disconnect

### First Connection Fails
The server now answers `461 Unsupported Transport` to UDP requests so clients fall back to TCP at once. If a client still fails, force TCP (`ffplay -rtsp_transport tcp`).

## Version History

v2.3.0 and earlier are [@stedrow](https://github.com/stedrow)'s releases of the upstream project, listed here because the firmware still builds on that work. v3.0.0 onwards are this fork.

### v3.0.0
Major version: the streaming core is rewritten. Settings and WiFi credentials are preserved; see "Upgrading from 2.x" in the README.
- Capture and network send split into two tasks joined by pre-allocated frame queues (~1.5 s); non-blocking whole-frame sends on the raw socket
- I2S DMA ring 22.5 ms → 224 ms usable with overrun counting
- RTP sequence/timestamp advance for every captured frame; random non-zero start with RTP-Info
- SETUP answers 461 to non-TCP transports and echoes the interleaved channel; keepalive replies carry Session; session timeout 60 s
- Gain/HPF/AGC changes apply live (no I2S restart, no disconnect); per-sample gain ramp
- Web UI served from flash (no 25 KB heap String per page), fixed-size log ring, Stream Health row, WiFi power save setting, CPU choices 80/160/240
- Defaults: CPU 80 MHz, `CORE_DEBUG_LEVEL=1`; thermal trip needs 3 consecutive readings; loop() yields
- Single-LED init (library default clocked a 25-LED matrix)

### v2.3.0
- Socket ownership model — Core 1 exclusively owns WiFiClient during streaming
- Confirmed task exit via FreeRTOS semaphore (prevents double-task creation)
- In-stream RTSP processing (TEARDOWN + keepalive on Core 1)
- LED ownership guards, log buffer spinlock, memory barriers
- Proactive WiFi disconnect handling
- Default CPU frequency lowered to 160MHz
- Copy logs button in Web UI

### v2.2.0
- RTSP receive buffer drain on disconnect
- Large RTSP session timeout (86400s)
- Write failure tolerance (100 consecutive failures before disconnect)
- Auto-recovery disabled by default (false positive prevention)
- NTP time sync (EST timestamps)
- Configurable LED mode (Off / Static / Level)
- Blue = ready, green = streaming LED colors
- Disconnect diagnostics (session duration, dropped packets, RSSI)

### v2.1.0
- Lock-free pointer handoff (removed mutex)
- Fixed first-connection race condition (VLC probe)
- Fixed `i2s_read` blocking, Core 1 heap contention
- mDNS discovery (`atomecho.local`)
- Automatic Gain Control (AGC)
- LED audio level indicator
- RTSP idle timeout (60s)
- Periodic heap monitoring
- Removed OTA (serial flash only)
- Default gain 3.0x, HPF cutoff 300Hz

### v2.0.0
- Dual-core architecture

### v1.0.0
- Initial release
