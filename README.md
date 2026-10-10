# M5Stack Atom Echo RTSP Microphone for BirdNET-Go

Firmware that turns an **M5Stack Atom Echo** into a network microphone: it streams the built-in microphone as an RTSP feed for [BirdNET-Go](https://github.com/tphakala/birdnet-go) or any RTSP client.

<p align="left">
  <img src="https://shop.m5stack.com/cdn/shop/files/3_e4ea519e-765f-4f30-aad1-7855ff9f8744_1200x1200.jpg" alt="M5Stack Atom Echo" width="300">
</p>

**Buy**: [M5Stack Store](https://shop.m5stack.com/products/atom-echo-smart-speaker-dev-kit) | [Amazon](https://www.amazon.com/M5Stack-Atom-Echo-Smart-Speaker/dp/B0C7QSVPB2)

This repository is a fork of [stedrow/birdnetgo-m5stack-atom-echo-rtsp-mic](https://github.com/stedrow/birdnetgo-m5stack-atom-echo-rtsp-mic), which did the Atom Echo port, the Web UI, the browser flasher and the release automation. Version 3.0.0 rewrote the streaming core on top of that work and 4.0.0 replaced the Web UI and the WiFi setup. Thank you, [@stedrow](https://github.com/stedrow).

## Features

- Capture and network send run in separate tasks joined by a send queue of up to 3 s of audio, so WiFi stalls shorter than the queue lose nothing. RTP frames are always sent whole.
- RTP sequence numbers and timestamps advance for every captured frame, so a frame that does get dropped reaches BirdNET-Go as a gap instead of a click.
- 32 kHz by default, which carries the whole 0 to 15 kHz band BirdNET analyses. A DC blocker, a 2nd-order high-pass filter (300 Hz by default), manual gain applied live with a per-sample ramp, and optional automatic gain control.
- Web UI: signal level, stream and WiFi diagnostics, settings with validation, the log, and firmware updates. One 11 KB page, English only.
- Listen to the microphone in the browser, with a live spectrogram, while BirdNET-Go keeps streaming.
- Firmware updates over WiFi. A new version that restarts before it has run for a minute with WiFi is rolled back to the previous one.
- Prometheus metrics at `/metrics`: frames sent and dropped, stalls, WiFi drops, RSSI, temperature, signal level.
- WiFi setup from the browser flasher over USB right after installing, or through a built-in captive portal; joins the strongest access point for the network name (mesh systems, repeaters).
- Reports why the device last restarted, including the task and backtrace after a crash, why WiFi last dropped, and a microphone that stopped delivering signal.
- Recovers by itself: restarts the audio driver if capture stalls, restarts the device after 10 minutes without WiFi.
- Thermal protection that trips only after 3 consecutive minutes over the limit, and a WiFi power save option.
- Configurable device name for mDNS (`atomecho.local` by default) and DHCP, so several microphones can share a network.
- No libraries beyond the Arduino core.

## Quick Start

### 1. Flash
```bash
pio run --target upload
```
Or use the browser flasher in `flash/` (published with GitHub Pages), which installs a release over USB from Chrome or Edge.

### 2. Connect to WiFi
With the browser flasher, the installer asks for your WiFi network once the firmware is written and sends it to the device over USB. Otherwise:

On first boot the LED turns cyan and the device opens an open WiFi network named `ESP32-RTSP-Mic-AP`. Join it with a phone or laptop; the setup page opens by itself (or browse to `http://192.168.4.1/`). Pick your network and enter its password. The device joins it and restarts. Only 2.4 GHz networks work.

The LED turns blue once the device is on your network and ready.

### 3. Stream
```bash
ffplay -rtsp_transport tcp rtsp://atomecho.local:8554/audio
# or
vlc rtsp://atomecho.local:8554/audio
```

**BirdNET-Go**: set the audio source to `rtsp://atomecho.local:8554/audio`

**Web UI**: `http://atomecho.local/`

BirdNET-Go running in Docker usually cannot resolve `.local` names. Use the device's IP address there (the Web UI and the serial log show it) and reserve that address for the device in your router's DHCP settings, so it does not change.

## Recommended Settings

| Setting | Default | Notes |
|---------|---------|-------|
| Sample rate | 32 kHz | Carries everything up to 16 kHz. At 16 kHz the stream stops at 8 kHz and loses the high songs of kinglets, crests and many warblers |
| Gain | 3.0x | Applied live. Aim for 30 to 70 % peak level in the Signal level card |
| Automatic gain control | Off | Turn on when birds call from very different distances |
| High-pass filter | On, 300 Hz | Removes wind and traffic rumble; lower it for large owls or bitterns |
| Frame size | 1024 samples | 32 ms per packet at 32 kHz |
| CPU clock | 80 MHz | Handles 48 kHz and runs coolest |
| WiFi power save | Off | Cuts heat. At 32 kHz it should keep up when the access point's DTIM period is 1 or 2. Switches itself off if the stream drops frames |

## LED

| Colour | Meaning |
|--------|---------|
| Yellow | Starting up, or the WiFi link is down |
| Cyan | WiFi setup portal running, or the button is being held |
| Blue | Ready, no client streaming |
| Green | Streaming |
| Red | Thermal protection stopped the stream (or clipping, in signal level mode) |
| Orange, dim green, dim purple | Signal level mode: hot, low, very quiet |

Yellow and the thermal red show even with the LED set to Off.

## Button

Hold the front button for 5 s to forget the WiFi network and restart into the setup portal, for example to move the device to another network. The LED turns cyan after 1 s; let go before 5 s to cancel. The Web UI's "Change WiFi network" button does the same.

## Updating

### From 3.0.x or 2.x
Versions before 4.0.0 cannot install firmware over WiFi, so install 4.0.0 once over USB. WiFi credentials and settings are kept.

- Stored settings win over the new defaults. A device that ran 16 kHz keeps it; choose 32 kHz in the Audio card (or press Restore defaults) to get the band above 8 kHz. A stored 8 kHz is replaced by 32 kHz, because 8 kHz clocks the microphone below its specified range.
- The HTTP API changed. The old `/api/*` endpoints are gone; see [the API reference](docs/DETAILS.md#http-api).
- The auto recovery setting is gone; a capture watchdog replaces it.
- Yellow LED now also means the WiFi link is down.

### From 4.0.0 on
Download `firmware.bin` from the [releases page](https://github.com/foliveira/birdnetgo-m5stack-atom-echo-rtsp-mic/releases) and install it in the Web UI's Firmware update card, or from a shell:
```bash
curl -F firmware=@firmware.bin http://atomecho.local/api/update
```
The device checks the whole image before switching to it. The new version then has to keep WiFi for a minute; if it restarts before that (a crash, or a WiFi join that fails), the device goes back to the previous version and shows "update rolled back" as its last restart.

## Monitoring

`/metrics` serves Prometheus text format. A scrape job:
```yaml
scrape_configs:
  - job_name: atomecho
    static_configs:
      - targets: ['192.168.1.50']   # the device's address
```
Use the device's IP address or `<name>.local` as the target: requests addressed to other names are refused (see Security). The metric list is in [docs/DETAILS.md](docs/DETAILS.md#metrics).

## Security

The Web UI and API have no password. Anyone who can reach the device can change its settings, reboot it, listen to it or install firmware, so keep it on a network you trust. Changes that a browser sends from another site's page are refused, and so is every request addressed to a name other than the device's IP address, its name, `<name>.local`, or its name under a local-only domain (`.lan`, `.home`, `.home.arpa`, `.internal`, `.localdomain`, `.fritz.box`). That stops web pages from changing the device or reading from it, including through DNS rebinding; open the device by one of those names.

## Building

```bash
pio run                      # Build
pio run --target upload      # Flash over USB
pio device monitor -b 115200 # Serial monitor
sh test/host/run.sh          # Unit tests of the DSP, RTSP parsing, JSON and metrics writers, Host check, Improv (needs a C++ compiler)
node test/webui/check_keys.mjs  # Checks that the page, the firmware and the mock device use the same API keys
```

The first build installs pioarduino's ESP32 platform (Arduino-ESP32 3.3.12 on ESP-IDF 5.5.5) and rebuilds the core's ESP-IDF libraries with the `custom_sdkconfig` options in `platformio.ini`: about 7 GB of downloads and tools, and about 4 minutes on a 7-core machine. Later builds reuse the libraries as long as `sdkconfig.defaults`, which the build writes, stays in place. PlatformIO shares the libraries between projects, so a project on the same platform version without these options makes them reinstall, and the next build here rebuilds them.

`web/index.html` is the Web UI; the build gzips it into the firmware. To work on it without a device, run `node test/webui/mock_server.mjs` and open `http://localhost:8080/`.

## Documentation

- [Architecture and troubleshooting guide](docs/DETAILS.md): pipeline, WiFi behaviour, HTTP API, metrics, audio tuning, troubleshooting, version history.

## Acknowledgments

- [stedrow/birdnetgo-m5stack-atom-echo-rtsp-mic](https://github.com/stedrow/birdnetgo-m5stack-atom-echo-rtsp-mic) by [@stedrow](https://github.com/stedrow), the project this repository is forked from: the M5Stack Atom Echo port, PDM microphone support, Web UI, thermal protection, browser flasher and release workflow all come from there. Thank you for the excellent work.
- [birdnetgo-esp32-rtsp-mic](https://github.com/Sukecz/birdnetgo-esp32-rtsp-mic) by [@Sukecz](https://github.com/Sukecz), the original ESP32 RTSP microphone that stedrow's port is based on.
- M5Stack for the Atom Echo hardware
- [BirdNET-Go](https://github.com/tphakala/birdnet-go) community
