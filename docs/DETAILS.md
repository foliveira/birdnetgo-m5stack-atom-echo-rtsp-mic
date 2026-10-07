# Architecture & Troubleshooting Guide

This is a fork of [stedrow/birdnetgo-m5stack-atom-echo-rtsp-mic](https://github.com/stedrow/birdnetgo-m5stack-atom-echo-rtsp-mic). The Atom Echo port, PDM microphone support and thermal protection described below come from that project; v3.0.0 rewrote the streaming pipeline on top of it and v3.1.0 the Web UI and WiFi handling.

## Hardware

- **Board**: M5Stack Atom Echo (ESP32-PICO-D4, no PSRAM)
- **Microphone**: SPM1423 PDM MEMS (built-in). Its clock must stay within 1.0 to 3.25 MHz; the PDM clock is 64 × the sample rate, which limits the rate to 16 to 48 kHz.
- **Audio**: 16-bit PCM mono, 32 kHz by default
- **Streaming**: RTSP with RTP interleaved over TCP, port 8554, path `/audio`

### Pins
```
G33  PDM clock to the microphone (also the speaker amplifier's LRCLK)
G23  PDM data from the microphone
G19  speaker amplifier bit clock: unused, held low
G22  speaker amplifier data: unused, held low
G27  SK6812 RGB LED
G39  front button, low while pressed
```

## Architecture

### Pipeline

Three actors, two FreeRTOS queues:

- **Capture task** (core 1, priority 20, runs from boot): `i2s_read` → DC blocker → high-pass filter → ramped gain/AGC → complete RTP frame (`$` interleave header + RTP header + big-endian L16) written into a pre-allocated `TxFrame` → pointer pushed to `readyQ`. It always meters the signal, so the Web UI level works without a client, and it is the only LED writer. It does not print: a full UART buffer would block it.
- **Sender task** (core 0, priority 8, created per RTSP session): pops frames from `readyQ`, sends them on the raw lwIP socket with `send(MSG_DONTWAIT)` + `select()`, returns them to `freeQ`. It always finishes a frame it has started, so the interleaved framing cannot desynchronise. It answers in-stream RTSP requests at frame boundaries and skips interleaved RTCP. If the socket accepts no bytes for 10 s the client is considered gone.
- **Arduino `loop()`** (core 1, priority 1; it shares core 1 with capture): RTSP negotiation, Web UI, WiFi supervision, diagnostics. It is the only user of the `WiFiClient` object and closes it only after the sender has confirmed exit through a semaphore.

Buffering: the I2S DMA ring is 8 × 512 samples, of which 7 (112 ms at 32 kHz) can wait for the capture task. The frame pool in front of the socket aims at 3 s of audio, because a single lwIP retransmission timeout is at least 1.5 s. Frames are allocated one at a time until 80 KB of heap is left or the pool reaches 128 KB (63 frames of 1024 samples, 2 s at 32 kHz). The boot log's `Frame pool:` line gives the frame count and how many milliseconds of audio it holds. When the pool is exhausted the capture task recycles the oldest queued frame and counts a drop; RTP sequence and timestamp still advance, so the receiver sees a gap rather than a time-compressed splice.

The DSP (`src/AudioDsp.h`) has no Arduino dependencies and is unit-tested on the host:
- A 10 Hz DC blocker always runs first; the PDM decoder's DC offset would otherwise be multiplied by the gain.
- The high-pass filter is a 2nd-order Butterworth biquad.
- Gain ramps linearly over each block, so gain and AGC changes do not click.
- Samples are rounded to nearest and clipped to 16 bits; blocks that clip are counted.
- AGC attack and release are defined per second, so they do not depend on sample rate or frame size.

A capture watchdog in `loop()` reinstalls the I2S driver when the capture task has not completed a read for 3 s (at most once every 30 s).

### Cross-Core Safety
- FreeRTOS queues carry frame pointers between cores; frames are allocated once.
- `portMUX_TYPE` spinlocks guard the fixed-size log ring and the HPF coefficient hand-off (no heap inside critical sections).
- Xtensa `memw` barriers on `isStreaming`, `stopStreamRequested`, `capturePauseRequested`.
- Sample rate and frame size changes park the capture task (`pauseCapture`), reinstall I2S, reallocate the pool and resume; the session ends and the client reconnects. If the new pool does not fit in memory, the previous values come back. Gain, HPF, AGC and LED changes apply live.

### WiFi
- **Joining**: the device scans all channels and joins the strongest access point that carries the stored network name. The ESP-IDF default joins the first one it finds, often a distant mesh node.
- **Over USB (Improv)**: the firmware answers [Improv Wi-Fi](https://www.improv-wifi.com/serial/) on the serial port, in the setup portal and in normal operation. Right after installing, the browser flasher asks for the network, sends it to the device over USB and then shows a link to the Web UI. A network that cannot be joined within 20 s changes nothing; one that joins replaces the stored network and the device restarts. While the flasher shows its network list it asks for a new scan every 3 s; the device answers from its last scan for 15 s, because each scan takes the radio off its channel for 1 to 4 s.
- **Setup portal**: with no stored network, or when the stored one cannot be joined within 60 s at boot, the device opens the open network `ESP32-RTSP-Mic-AP` with a captive page that lists nearby networks. With a stored network it retries that network every 30 s while nobody is connected to the portal and restarts as soon as it answers (a router that boots slower than the device after a power cut), and restarts after 3 idle minutes. Without one it waits indefinitely. A network entered in the portal that cannot be joined does not replace the stored one. Credentials live in the same NVS slot WiFiManager used, so devices upgraded from 2.x and 3.0 keep them.
- **At runtime**: a WiFi event handler tracks the link. `loop()` logs each drop with its reason, nudges a reconnect every 15 s, and restarts the device after 10 minutes without a link. An RTSP session survives a short drop: when the link returns with the same address the TCP connection is still valid, and the sender ends the session itself if the client stays unreachable for 10 s. The LED is yellow while the link is down.
- **Power save**: with WiFi power save on, the radio sleeps between beacons and TCP acknowledgements arrive once per DTIM period. The 5760-byte TCP send window then carries 5760 B per beacon interval, enough for 16 kHz with DTIM 1 but not for higher rates. The firmware turns power save off until the next reboot when a session drops 10 frames.

### RTSP
- One client at a time. A second client gets no answer until the first disconnects.
- Methods: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN, GET_PARAMETER and SET_PARAMETER (the last two are keepalives). Other methods get `501 Not Implemented`.
- Only TCP interleaved transport is offered. SETUP with a UDP transport gets `461 Unsupported Transport`, so clients fall back to TCP at once.
- PLAY without a SETUP on the same connection gets `455 Method Not Valid in This State`. PLAY gets `503 Service Unavailable` when the frame pool could not be allocated or the previous session is still closing.
- SETUP advertises a 60 s session timeout, so clients send a keepalive about every 30 s. A session that is receiving audio is never ended for a missing keepalive. A client that connects and sends nothing for 60 s before PLAY is disconnected.

### Live listening
`GET /api/listen` hands its HTTP connection to a listener task (core 0, priority 5, below the RTP sender) that sends the audio as an endless WAV file. The capture task copies each processed block, byte-swapped to little-endian, into a FreeRTOS stream buffer of 250 ms (at least two blocks); the listener task forwards it with non-blocking writes. The listener takes nothing from the RTSP frame pool, so it does not disturb a BirdNET-Go session; when the listener falls behind, blocks are skipped for it alone. One listener at a time. It needs the buffer plus 32 KB of free heap, and ends when the sample rate or frame size changes (its WAV header would be wrong), when thermal protection trips, or when the listener accepts no data for 5 s.

### Power and Heat
Controllable consumers, largest first: the WiFi receiver with power save off (~80 to 100 mA), CPU 160 vs 80 MHz (~10 mA), transmit power (a few mA at this duty cycle), the LED (<2 mA at the brightness used). The internal temperature sensor is uncalibrated and reads high; thermal protection therefore requires three consecutive readings, one per minute, at or over the limit.

### Restart reasons and crash reports
The Device card and the boot log show why the device last restarted: power on, reset pin, brownout, crash, watchdog, or the note the firmware left before restarting itself (`scheduled reboot`, `Web UI reboot`, `defaults restored from the Web UI`, `firmware update`, `WiFi network forgotten from the Web UI`, `WiFi network forgotten (button)`, `recovery: WiFi down for 10 min`, `recovery: sender task stuck`, `joined a network from the setup portal`, `joined a network sent over USB`, `setup portal: stored network is back`, `setup portal idle for 3 minutes`).

After a crash or watchdog reset the panic handler has written a core dump to flash. The boot log then has a line like this, and the firmware erases the dump once it has logged it, so an old dump is never reported for a later reset:
```
Crash: task AudioCapture, PC 0x400d2f1c, backtrace: 0x400d2f19 0x400d3a2e ...
```
Decode it against the ELF file of the exact same build (releases include `firmware.elf`):
```bash
~/.platformio/packages/toolchain-xtensa-esp32/bin/xtensa-esp32-elf-addr2line -pfiaC \
  -e .pio/build/m5stack-atom/firmware.elf 0x400d2f19 0x400d3a2e
```

### Firmware updates
`POST /api/update` writes the uploaded image to the other OTA slot with the core's Update library. The boot slot changes only after the whole image has been verified (checksum, SHA-256, chip type), so a truncated or wrong file leaves the running firmware in place. The new firmware boots in the "pending verify" state and the sketch confirms it after it has run for a minute with WiFi. If it restarts before that, the bootloader marks it invalid and boots the previous firmware, which then reports `update rolled back after <reason>` as its last restart. Rollback needs a bootloader built with rollback support; the one in this project's releases (Arduino-ESP32 2.0.17) has it, and only a USB flash replaces the bootloader.

## Audio Tuning

### Signal Levels
The Signal level card shows the loudest peak of the last 3 s after gain.
- **Target**: 30 to 70 % (about -10 to -3 dBFS)
- **Clipping** distorts calls: reduce gain or enable AGC
- With the LED in signal level mode: bright green is a good level, orange is hot, red is clipping, dim green is low, dim purple is very quiet

### Sample Rate
BirdNET analyses 0 to 15 kHz. 32 kHz (the default) carries up to 16 kHz and clocks the microphone at 2.048 MHz, in the middle of its range. 16 kHz stops at 8 kHz. 44.1 and 48 kHz add nothing BirdNET uses and need 50 % more bandwidth.

### AGC vs Manual Gain
- **AGC off**: constant, predictable level; BirdNET sees the scene as it is.
- **AGC on**: for deployments where bird distance varies a lot. AGC multiplies on top of the manual gain.

### High-Pass Filter
300 Hz removes most wind and traffic rumble. A few birds call lower (large owls, bitterns, some doves); lower the cutoff if they matter.

### Frame Size (samples per RTP packet)

Stream stability does not depend on this setting: the send queue holds the same amount of audio whatever the frame size. It trades packet rate (WiFi airtime, ACK traffic) against per-packet latency.

| Size | Packet at 32 kHz | Packets/s | Notes |
|------|------------------|-----------|-------|
| 256 | 8 ms | 125 | Most WiFi frames |
| 512 | 16 ms | 62 | |
| **1024** | **32 ms** | **31** | **Default** |
| 2048 | 64 ms | 16 | |
| 4096 | 128 ms | 8 | Fewest packets; each spans several TCP segments |

## Web UI

One page (`web/index.html`, gzipped into the firmware at build time) that polls `/api/state` once a second while it is visible.

- **Signal level**: peak meter, clipping count, AGC factor, and a warning when the microphone delivers constant samples for 10 s (a working microphone always shows some noise). **Listen** plays the stream in the browser and draws a spectrogram (newest on the right, up to half the sample rate), so the effect of gain and the high-pass filter, and whether high calls come through, can be checked by ear and by eye. Listening adds a second audio stream on the WiFi link while it runs.
- **Stream**: client, session length or why the last one ended, format, send queue depth, dropped frames, I2S overruns, longest stall, frames sent, connections, who is listening.
- **Device**: uptime, last restart reason, WiFi network and RSSI, access point (BSSID, channel), WiFi drops with the last reason, address and mDNS name, power save state, free memory, CPU clock, temperature, thermal guard.
- **Audio**, **Network and power**, **Protection and maintenance**: settings forms with Save and Undo. Polling never overwrites a field being edited; every field is validated before any is applied.
- **Actions**: restart audio, stop or start the RTSP server, reboot, restore defaults (keeps the WiFi network), change WiFi network.
- **Firmware update**: installs a `firmware.bin` with upload progress.
- **Log**: the last 48 lines, in UTC once the clock has synced (`+hh:mm:ss` is time since boot before that).

## HTTP API

POST bodies are form-encoded. Replies are JSON unless noted. There is no authentication. Two checks keep web pages from other sites out:
- Every request must be addressed (`Host` header) to the device's IPv4 address, its name, `<name>.local`, or its name under `.lan`, `.home`, `.home.arpa`, `.internal`, `.localdomain` or `.fritz.box`, with any port. Anything else gets `403`. This defeats DNS rebinding, where a page points a name of its own at the device's address.
- POST requests whose `Origin` header names another site get `403`.

### `GET /api/state`
```json
{"fw": "3.1.0", "cfg": {...}, "live": {...}}
```
`cfg` holds the settings, with the same keys `/api/settings` accepts:

| Key | Values |
|-----|--------|
| `rate` | 16000, 22050, 24000, 32000, 44100, 48000 |
| `buffer` | 256 to 8192 samples per packet |
| `gain` | 0.1 to 100 |
| `hpf`, `hpf_hz` | high-pass filter on/off, cutoff 10 to 10000 Hz |
| `agc` | automatic gain control on/off |
| `tx` | WiFi transmit power, -1 to 19.5 dBm (snapped to the radio's steps) |
| `ps` | WiFi power save on/off |
| `cpu` | 80, 160, 240 MHz |
| `led` | 0 off, 1 status colour, 2 signal level |
| `oh`, `oh_limit` | thermal protection on/off, limit 30 to 95 °C in steps of 5 |
| `sched`, `sched_h` | scheduled reboot on/off, every 1 to 168 hours |
| `name` | device name: 1 to 31 lowercase letters, digits or hyphens, no hyphen at either end |

`live` holds the status the page shows: `uptime`, `restart_reason`, `ip`, `ssid`, `rssi`, `bssid`, `channel`, `wifi_drops`, `wifi_last_drop`, `wifi_last_drop_s`, `ps_suspended`, `heap_kb`, `heap_min_kb`, `cpu_mhz`, `temp_c`, `temp_max_c`, `server`, `streaming`, `client`, `session_s`, `end_reason`, `connects`, `plays`, `listener` (address of whoever listens, empty when nobody), `rate`, `frame_ms`, `sent`, `dropped`, `overruns`, `read_errors`, `queue`, `queue_peak`, `queue_cap`, `stall_max_ms`, `stalls`, `peak_pct`, `hold_pct`, `clips`, `agc_x`, `mic_flat`, `oh_latched`, `oh_sensor_fault`, `oh_note`, `log_seq`. Stream counters restart with every session. Unknown values are `null`.

### `POST /api/settings`
Any subset of the `cfg` keys. Booleans take `1`/`0`, `on`/`off` or `true`/`false`. All keys are validated before any is applied; the reply is the new state, or `400 {"error": "gain: must be 0.1 to 100"}`. Changing `rate` or `buffer` restarts the audio pipeline and ends the session; if the new frame pool does not fit in memory the previous values are restored and the reply is `500`.
```bash
curl -d gain=4 -d agc=0 http://atomecho.local/api/settings
```

### `POST /api/action`
`name=` one of `restart_audio`, `server_on`, `server_off`, `clear_thermal`, `reboot`, `defaults`, `forget_wifi`. `server_on` answers `409` while the thermal latch is set (use `clear_thermal`). `reboot`, `defaults` and `forget_wifi` restart the device half a second after replying.

### `POST /api/update`
Multipart upload (`multipart/form-data`) of a `firmware.bin` (any field name). Replies `{"ok": true}` and restarts into the new firmware, or `400` with the Update library's error (for example `Wrong Magic Byte` for a file that is not an ESP32 image), or `no firmware file in the request` for any other body. The RTSP session is stopped before writing. If the upload connection drops, the partial update is discarded.

### `GET /api/listen`
Live audio as a WAV file that does not end (16-bit mono PCM at the current sample rate; the header announces about 9 hours). Play it with `<audio src>` or save it: `curl -o garden.wav http://atomecho.local/api/listen` records until you stop curl. One listener at a time: a request from another address gets `409`, while a new request from the address that is listening replaces its stream (a reloaded page). `503` when memory is short, capture is not running or the thermal latch is set. A request for a byte range that does not start at 0 gets `416`: browsers ask for one to resume a dropped stream, and a live stream cannot be resumed part-way.

### `GET /api/logs?since=N`
Plain text: the log lines after line number `N` (at most the last 48). The `X-Log-Seq` header holds the number of the newest line; pass it as `since` next time.

### `GET /metrics`
Prometheus text format, see below.

## Metrics

Counters are totals since boot; they do not restart with each session.

| Metric | Type | Meaning |
|--------|------|---------|
| `rtspmic_info{version,name}` | gauge | Always 1; firmware version and device name as labels |
| `rtspmic_uptime_seconds` | gauge | Seconds since boot |
| `rtspmic_wifi_rssi_dbm` | gauge | Signal strength of the access point |
| `rtspmic_wifi_channel` | gauge | WiFi channel |
| `rtspmic_wifi_disconnects_total` | counter | WiFi link losses |
| `rtspmic_heap_free_bytes`, `rtspmic_heap_min_free_bytes` | gauge | Free heap now and its lowest value since boot |
| `rtspmic_temperature_celsius` | gauge | Chip sensor, uncalibrated (absent while the sensor fails) |
| `rtspmic_thermal_latched` | gauge | 1 while thermal protection holds the server off |
| `rtspmic_rtsp_server_enabled` | gauge | 1 while the RTSP server accepts clients |
| `rtspmic_streaming` | gauge | 1 while a client receives audio |
| `rtspmic_rtsp_connections_total` | counter | RTSP connections accepted |
| `rtspmic_rtsp_sessions_total` | counter | Sessions started (PLAY) |
| `rtspmic_sample_rate_hertz` | gauge | Sample rate the microphone runs at |
| `rtspmic_frames_sent_total` | counter | RTP frames sent |
| `rtspmic_frames_dropped_total` | counter | Frames dropped because the network fell behind |
| `rtspmic_i2s_overruns_total` | counter | DMA buffers lost because capture fell behind |
| `rtspmic_i2s_read_errors_total` | counter | Failed or short I2S reads |
| `rtspmic_send_stalls_total` | counter | Times the socket accepted no data |
| `rtspmic_send_stall_max_seconds` | gauge | Longest send stall in the current session |
| `rtspmic_send_queue_frames`, `rtspmic_send_queue_capacity_frames` | gauge | Frames waiting to be sent, and the queue's size |
| `rtspmic_audio_peak_ratio` | gauge | Loudest peak of the last 3 s after gain, 1 = full scale |
| `rtspmic_audio_clipped_blocks_total` | counter | Audio blocks that clipped |
| `rtspmic_agc_multiplier` | gauge | AGC factor (1 when off) |
| `rtspmic_microphone_flat` | gauge | 1 while the microphone delivers constant samples |

Useful alerts: `increase(rtspmic_frames_dropped_total[1h]) > 0` (audio lost), `rtspmic_microphone_flat == 1`, `rtspmic_streaming == 0` for 10 minutes (BirdNET-Go is not connected), `rtspmic_uptime_seconds < 300` (the device restarted; the Web UI says why).

## Troubleshooting

### LED is yellow
- At boot: the device is joining WiFi. After 60 s without success it opens the setup portal (cyan).
- Later: the WiFi link is down. The device keeps reconnecting and restarts after 10 minutes. The Device card shows the reason of the last drop once the link is back.

### LED is cyan
The setup portal is running: join `ESP32-RTSP-Mic-AP` and pick a network, or connect the device to a computer and use the browser flasher's "Connect" button to send it a network over USB. If it appeared after a power cut, the device keeps retrying the stored network every 30 s and returns to it by itself once the router is up.

### LED is red (not streaming)
Thermal protection stopped the stream. The Web UI shows when and at what temperature. Let the device cool, then press Resume streaming. To run cooler: WiFi power save on, CPU 80 MHz, LED off, shade.

### No audio or low level
1. Check the Signal level card. If it warns that the microphone delivers no signal, the samples are constant: a hardware fault, or the microphone's data line.
2. Increase gain (try 5 to 10) or enable AGC.
3. Turn the high-pass filter off to compare.

### Audio clipping or distortion
1. Decrease gain.
2. Enable AGC.
3. Aim for 30 to 70 % in the Signal level card.

### Stream drops or clicks
1. Read the Stream card. Dropped frames mean the client or WiFi could not take the send queue's worth of audio (the boot log's `Frame pool:` line says how much that is). I2S overruns mean the capture task was starved and should always be 0.
2. Check the WiFi signal: RSSI above -70 dBm. The Device card shows which access point the device is on; a lower transmit power is only for strong links.
3. If WiFi power save is on and frames drop, the firmware turns it off until reboot after 10 drops and says so in the log: the access point's DTIM period is too long for the TCP send window.
4. On serial, `loop()` prints `[Stream] ...` statistics every 30 s during a session and `[Sender] session ended: <reason>` when it ends.

### First connection fails
SETUP over UDP is answered with `461 Unsupported Transport`, so clients fall back to TCP at once. If a client still fails, force TCP (`ffplay -rtsp_transport tcp`).

### Device restarted
Check "Last restart" in the Device card or the boot log. After a crash, decode the backtrace as described above and open an issue with it.

## Development

- `sh test/host/run.sh` builds and runs the host unit tests (DSP, RTSP request parsing, JSON and Prometheus writers, Host header check, Improv protocol) with the system C++ compiler.
- `node test/webui/mock_server.mjs [port]` serves the page with a fake device that mirrors the firmware's API (`MOCK_SCENARIO=latched` or `idle` for other states).
- `node test/webui/check_keys.mjs` checks that the page, `src/WebUI.cpp` and the mock use the same JSON keys.
- `node test/webui/ui_test.mjs` drives the page in headless Chrome against the mock (see the file's header for starting Chrome).

## Version History

v2.3.0 and earlier are [@stedrow](https://github.com/stedrow)'s releases of the upstream project, listed here because the firmware still builds on that work. v3.0.0 onwards are this fork.

### v3.1.0
Settings and WiFi credentials are preserved; see "Updating" in the README.
- Web UI rewritten: English only, one gzipped 10 KB page, a single JSON state endpoint polled once a second, settings validated before they are applied, cross-site POSTs and foreign host names (DNS rebinding) refused. The old `/api/*` endpoints are removed.
- Listen to the microphone in the Web UI with a live spectrogram, beside the RTSP stream (`/api/listen`)
- Firmware update from the Web UI (`/api/update`) with automatic rollback when the new version restarts within its first minute
- Prometheus metrics at `/metrics`
- WiFi setup from the browser flasher over USB (Improv Wi-Fi), right after installing
- WiFiManager, M5Atom and FastLED removed: built-in setup portal, LED driven by the core's `neopixelWrite`. The firmware went from not fitting the app partition with current libraries (1.48 MB) to 0.95 MB.
- Joins the strongest access point for the network name; RTSP sessions survive short WiFi drops; restart after 10 minutes without WiFi; the setup portal retries the stored network every 30 s
- Change the WiFi network from the Web UI or by holding the button for 5 s
- Device name setting for mDNS and DHCP (applied at once; DHCP sends it from its next request)
- Default sample rate 32 kHz (BirdNET's full band); 8 kHz removed (below the microphone's clock range)
- Always-on DC blocker, rounding instead of truncation, AGC time constants independent of rate and frame size; the speaker amplifier's pins are held low
- Frame pool allocated frame by frame with a 3 s target; capture watchdog restarts a stalled I2S driver (replaces the packet-rate auto recovery)
- RTSP: every pipelined request is answered, request bodies are skipped, 501 for unsupported methods, 455 for PLAY without SETUP, SET_PARAMETER keepalive
- Restart reason, crash backtrace, WiFi drop reason and BSSID, and a dead-microphone warning in the Web UI and log
- LED yellow while WiFi is down; faults show even with the LED off
- Log timestamps in UTC; uptime and scheduled reboots no longer break when `millis()` wraps after 49 days
- Host unit tests, a mock device and a browser test for the Web UI

### v3.0.0
Major version: the streaming core is rewritten. Settings and WiFi credentials are preserved.
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
