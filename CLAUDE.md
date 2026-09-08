# CLAUDE.md - AI Development Context

See `README.md` for project documentation, features, architecture, and usage.

## Key Files
- `src/esp32_rtsp_mic_birdnetgo.ino` — Main firmware (capture task, sender task, RTSP, I2S, settings)
- `src/WebUI.cpp` — Web interface (HTML/JS/CSS as one PROGMEM literal served with send_P, JSON API endpoints)
- `src/WebUI.h` — WebUI header
- `platformio.ini` — Build configuration and dependencies

## Critical Rules

### i2sShiftBits MUST be 0
- PDM microphones output 16-bit samples directly, no bit shifting needed
- This is hardcoded to 0 in the firmware — do not make it configurable
- If it gets set to any other value, all audio becomes zeros (e.g., `180 >> 11 = 0`)
- Web UI shows it as read-only: "0 bits (fixed for PDM)"

### Pipeline and Socket Ownership Model (v3.0)
- Three actors: capture task (core 1, priority 20, runs from boot), sender task (core 0, created per RTSP session), Arduino `loop()` (core 1, priority 1; `CONFIG_ARDUINO_RUNNING_CORE=1`, so it shares core 1 with capture).
- The capture task never touches the socket. It reads I2S, runs the DSP, writes complete RTP frames (interleave header + RTP header + big-endian PCM) into pre-allocated `TxFrame` slots and pushes pointers into `readyQ`. When `freeQ` is empty it recycles the OLDEST ready frame (bounded latency) and counts a drop. RTP seq/timestamp advance for every captured frame, sent or not.
- The sender task is the only task doing socket I/O during a session, on the raw lwIP fd with `send(..., MSG_DONTWAIT)` + `select()`. It always finishes a frame it has started (framing can never desync) and services in-stream RTSP requests (keepalive, TEARDOWN, RTCP skip) only at frame boundaries.
- `loop()` is the only user of the `WiFiClient` object. It closes the client only after the sender has confirmed exit via `senderExitSem`. To stop a session from `loop()` call `requestStreamStop(reason)`; on an unconfirmed exit it returns false and tears NOTHING down (socket stays open, handle stays set, reboot after 30 s). Never close the socket or touch the frame pool while `senderTaskHandle != NULL`.
- Sample rate / buffer size changes go through `restartI2S()`: it ends the session, parks the capture task (`pauseCapture()`), reinstalls I2S, reallocates the frame pool, resumes. Gain/HPF/AGC changes need no restart (read live by the capture task; HPF coefficients hand off under `hpfMux`).
- The capture task is the only LED writer after `setup()`.

### Cross-Core Safety
- Use FreeRTOS queues for inter-core data transfer (`freeQ`/`readyQ` carry frame pointers); never hand-rolled ring buffers.
- `portMUX_TYPE` spinlocks guard the fixed-size log ring (`logMux`) and the HPF coefficient hand-off (`hpfMux`). Never allocate heap inside a critical section.
- Xtensa `memw` barriers on flag transitions (`isStreaming`, `stopStreamRequested`, `capturePauseRequested`).
- Capture/sender tasks never call `simplePrintln`/`String`; they use `Serial.printf` and counters. `loop()` logs on transitions.

### Do Not Reintroduce
- `WiFiClient::write()` in the streaming path (it blocks up to 1 s per retry, 10 retries).
- Multiple TCP writes per RTP packet or "dropping" a partially written frame.
- `WiFiClient::flush()` after a write (on arduino-esp32 it discards the receive buffer).
- Tiny I2S DMA rings (`dma_buf_len=60`): the ring is 8 x 512 frames on purpose, and the I2S event queue must stay much deeper than `dma_buf_count` (RX_DONE events fill it and hide the overrun event).
- `CORE_DEBUG_LEVEL` above 1 in `platformio.ini` for release builds.
- CPU frequency options other than 80/160/240 (120 is not a valid PLL setting on ESP32).

## Debugging Tips

### If Audio Clicks or Drops
1. Open the Web UI Status card: "Stream Health" shows queue depth/peak, dropped frames, I2S overruns, longest send stall.
2. I2S overruns > 0 means the capture task was starved for > 224 ms (7 x 512-frame DMA buffers can wait; check what else runs on core 1).
3. Dropped frames > 0 means the client/WiFi could not take 1.5 s of audio; check RSSI, AP DTIM if WiFi power save is on.
4. Serial prints `[Capture] ...` stats every 30 s during a session and `[Sender] session ended: <reason>` on exit.

### If Audio Stops Working
1. Check serial for `Frame pool:` and `I2S ready` lines at boot; a failed pool allocation disables streaming and PLAY is answered with `503 Service Unavailable` (log: `PLAY refused: no frame pool`). A rate/buffer change whose pool does not fit reverts to the previous values.
2. Use "Defaults" button in Web UI if flash settings are corrupted (loadAudioSettings also clamps rate/buffer/gain).

### If System is Unstable or Hot
1. The ESP32 internal temperature sensor is uncalibrated and reads high; thermal protection trips only after 3 consecutive minutes at/above the limit.
2. Heat levers, largest first: WiFi Power Save ON (Advanced Settings; needs an AP with DTIM period 1 because the TCP send window is 5760 B; the firmware suspends it until reboot after 10 dropped frames), CPU 80 MHz, LED off.
3. Check WiFi RSSI (should be > -70 dBm) and free heap (should stay above ~80KB after the ~50KB frame pool).

## Build & Deploy
```bash
pio run                      # Build
pio run --target upload      # Upload
pio device monitor -b 115200 # Monitor
```

## Configuration Storage
Settings saved to flash in `audioPrefs` namespace via ESP32 Preferences library.
