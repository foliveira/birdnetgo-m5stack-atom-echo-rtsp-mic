# CLAUDE.md - AI Development Context

See `README.md` for project documentation and usage, and `docs/DETAILS.md` for the architecture, HTTP API and metrics.

## Key Files
- `src/esp32_rtsp_mic_birdnetgo.ino` — Main firmware: capture task, sender task, RTSP negotiation, I2S, settings, WiFi supervision, button, LED, firmware confirmation
- `src/App.h` — Everything the sketch shares with the other modules (settings, live state, actions). Add declarations here, not as loose `extern`s
- `src/WebUI.cpp` / `WebUI.h` — HTTP server: page, `/api/state`, `/api/settings`, `/api/action`, `/api/update`, `/api/logs`, `/metrics`, log ring
- `web/index.html` — The Web UI page. `scripts/embed_webui.py` gzips it into `webui_index.h` in the build directory before every build
- `src/WifiSetup.cpp` / `WifiSetup.h` — Joining the stored network and the setup portal (replaces WiFiManager)
- `src/RestartInfo.cpp` / `RestartInfo.h` — Restart reason, RTC note for the firmware's own restarts, core dump summary after a crash
- `src/AudioDsp.h`, `src/RtspRequest.h`, `src/JsonOut.h`, `src/PromOut.h`, `src/HostCheck.h` — Plain C++ (no Arduino), unit-tested on the host in `test/host`
- `test/webui/` — Mock device, browser test and API key check for the page
- `platformio.ini` — Build configuration; the platform is pinned

## Critical Rules

### No bit shift on PDM samples
- The PDM decoder delivers 16-bit samples; they are used as they are.
- There is no shift setting any more. Do not add one: any shift turns the audio into zeros (e.g. `180 >> 11 = 0`).

### Pipeline and Socket Ownership Model
- Three actors: capture task (core 1, priority 20, runs from boot), sender task (core 0, created per RTSP session), Arduino `loop()` (core 1, priority 1; `CONFIG_ARDUINO_RUNNING_CORE=1`, so it shares core 1 with capture).
- The capture task never touches the socket. It reads I2S, runs the DSP, writes complete RTP frames (interleave header + RTP header + big-endian PCM) into pre-allocated `TxFrame`s and pushes pointers into `readyQ`. When `freeQ` is empty it recycles the OLDEST ready frame (bounded latency) and counts a drop. RTP seq/timestamp advance for every captured frame, sent or not.
- The sender task is the only task doing socket I/O during a session, on the raw lwIP fd with `send(..., MSG_DONTWAIT)` + `select()`. It always finishes a frame it has started (framing can never desync) and services in-stream RTSP requests (keepalive, TEARDOWN, RTCP skip) only at frame boundaries.
- `loop()` is the only user of the `WiFiClient` object. It closes the client only after the sender has confirmed exit via `senderExitSem`. To stop a session from `loop()` (HTTP handlers run inside it) call `requestStreamStop(reason)`; on an unconfirmed exit it returns false and tears NOTHING down (socket stays open, handle stays set, reboot after 30 s). Never close the socket or touch the frame pool while `senderTaskHandle != NULL`.
- Sample rate / frame size changes go through `restartI2S()`: it ends the session, parks the capture task (`pauseCapture()`), reinstalls I2S, reallocates the frame pool, resumes, and puts the previous values back if the pool does not fit. Gain/HPF/AGC/LED changes need no restart (read live by the capture task; HPF coefficients hand off under `hpfMux`).
- The frame pool is allocated frame by frame (the heap's largest block is far smaller than its total): target `TARGET_QUEUE_MS` (3 s), cap 128 KB, stop when 80 KB of heap is left.
- The capture task is the only LED writer after `setup()`. The LED is driven with the core's `neopixelWrite()`.

### Cross-Core Safety
- Use FreeRTOS queues for inter-core data transfer (`freeQ`/`readyQ` carry frame pointers); never hand-rolled ring buffers.
- `portMUX_TYPE` spinlocks guard the fixed-size log ring (`logMux`) and the HPF coefficient hand-off (`hpfMux`). Never allocate heap inside a critical section.
- Xtensa `memw` barriers on flag transitions (`isStreaming`, `stopStreamRequested`, `capturePauseRequested`).
- The capture task does not print (a full UART buffer would block it; its DMA slack is 112 ms at 32 kHz) and never uses `String`. The sender uses `Serial.printf` at session start and end only. `loop()` logs transitions with `simplePrintln` and prints the 30 s `[Stream]` statistics.
- Read millisecond stamps that the capture task writes (`lastCaptureOkMs`, `lastMicSignalMs`) BEFORE calling `millis()`: the other order can pair a newer stamp with an older "now" and the unsigned difference wraps.

### Web UI and API
- The page lives in `web/index.html`; never paste HTML into C++ strings.
- `sendState()` / `parseSetting()` in `WebUI.cpp`, the page and `test/webui/mock_server.mjs` must use the same keys, messages and status codes. `node test/webui/check_keys.mjs` checks the keys.
- State changes only through POST, and every POST handler starts with `fromThisPage()` (Host and Origin checks). GET handlers are registered through `deviceHostOnly()` (Host check, against DNS rebinding) and must not change state.
- WebServer calls an upload callback for non-multipart POST bodies too (raw mode), when `web.upload()` is invalid: check the Content-Type first.
- `/api/settings` validates every key before applying any.
- Replies are built in fixed buffers (`JsonOut`, `PromOut`); no `String` concatenation of whole replies.

### Restarts and firmware updates
- HTTP handlers never restart directly: `scheduleReboot(kind, delayMs, note)` lets the reply go out first. Elsewhere use `restartWithNote(note)`. The note is the restart reason on the next boot: a string literal under 40 characters.
- After a Web UI update the new firmware runs on probation (`verifyRollbackLater()` returns true) until `confirmFirmware()` accepts it after 60 s of uptime with WiFi. Any restart before that rolls back to the previous firmware, including the setup portal's restarts. Keep setup and the first minute free of deliberate restarts.

### Do Not Reintroduce
- `WiFiClient::write()` in the streaming path (it blocks up to 1 s per retry, 10 retries).
- Multiple TCP writes per RTP packet or "dropping" a partially written frame.
- `WiFiClient::flush()` after a write (on arduino-esp32 it discards the receive buffer).
- Tiny I2S DMA rings (`dma_buf_len=60`): the ring is 8 x 512 frames on purpose, and the I2S event queue must stay much deeper than `dma_buf_count` (RX_DONE events fill it and hide the overrun event).
- `CORE_DEBUG_LEVEL` above 1 in `platformio.ini` for release builds.
- CPU frequency options other than 80/160/240 (120 is not a valid PLL setting on ESP32).
- Sample rates below 16 kHz: the PDM clock (64 x rate) would fall below the SPM1423's 1.0 MHz minimum.
- Libraries for what the core already does. FastLED, M5Atom and WiFiManager cost ~500 KB of flash. The app partition is 1.25 MB, and devices updated over the Web UI can only get a new partition table over USB.
- Prints or `String` in the capture task.

## Debugging Tips

### If Audio Clicks or Drops
1. Open the Web UI Stream card: send queue depth, dropped frames, I2S overruns, longest stall.
2. I2S overruns > 0 means the capture task was starved for > 112 ms at 32 kHz (7 x 512-sample DMA buffers can wait; check what else runs on core 1).
3. Dropped frames > 0 means the client/WiFi could not take the frame pool's worth of audio (the boot log's `Frame pool:` line says how much); check RSSI, and the AP's DTIM period if WiFi power save is on.
4. Serial prints `[Stream] ...` stats (with stack high-water marks) every 30 s during a session and `[Sender] session ended: <reason>` on exit. `/metrics` keeps since-boot totals.

### If Audio Stops Working
1. Check serial for `Frame pool:` and `I2S ready` lines at boot; a failed pool allocation disables streaming and PLAY is answered with `503 Service Unavailable` (log: `PLAY refused: no frame pool`). A rate/buffer change whose pool does not fit reverts to the previous values.
2. `Capture stalled` in the log means the watchdog reinstalled the I2S driver. `no signal from the microphone` means the samples are constant (hardware).
3. Use "Restore defaults" in the Web UI if flash settings are corrupted (`loadAudioSettings` also clamps every value).

### If the Device Restarts
1. "Last restart" in the Device card and the boot log give the reason; after a crash the log has a `Crash:` line with the task, PC and backtrace. Decode with `xtensa-esp32-elf-addr2line -pfiaC -e .pio/build/m5stack-atom/firmware.elf <addresses>` against the exact build.

### If System is Unstable or Hot
1. The ESP32 internal temperature sensor is uncalibrated and reads high; thermal protection trips only after 3 consecutive minutes at/above the limit.
2. Heat levers, largest first: WiFi power save ON (Network and power card; keeps up only with 16 kHz and an AP with DTIM period 1, because the TCP send window is 5760 B; the firmware suspends it until reboot after 10 dropped frames), CPU 80 MHz, LED off.
3. Check WiFi RSSI (should be > -70 dBm) and free heap (should stay near the 80 KB the frame pool leaves).

## Build, Test & Release
```bash
pio run                      # Build
pio run --target upload      # Upload over USB
pio device monitor -b 115200 # Monitor
sh test/host/run.sh          # Host unit tests (C++17 compiler)
node test/webui/check_keys.mjs   # API keys: page vs firmware vs mock
node test/webui/ui_test.mjs      # Page behaviour in headless Chrome (DevTools on :9222, see the file header)
```
Releases are tagged from `FW_VERSION` in the sketch when it changes on `main`.

## Configuration Storage
Settings are saved to flash in the `audio` Preferences namespace (`audioPrefs`). WiFi credentials are kept by the WiFi driver in its own NVS storage, where WiFiManager kept them.
