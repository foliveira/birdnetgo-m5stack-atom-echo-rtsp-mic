# M5Stack Atom Echo — RTSP Microphone for BirdNET-Go

A high-quality RTSP audio streaming server for the **M5Stack Atom Echo**, streaming live audio to [BirdNET-Go](https://github.com/tphakala/birdnet-go) or any RTSP-compatible client.

<p align="left">
  <img src="https://shop.m5stack.com/cdn/shop/files/3_e4ea519e-765f-4f30-aad1-7855ff9f8744_1200x1200.jpg" alt="M5Stack Atom Echo" width="300">
</p>

**Buy**: [M5Stack Store](https://shop.m5stack.com/products/atom-echo-smart-speaker-dev-kit) | [Amazon](https://www.amazon.com/M5Stack-Atom-Echo-Smart-Speaker/dp/B0C7QSVPB2)

## Features

- **Decoupled audio pipeline** — an always-on capture task (core 1) fills a 1.5 s frame queue; a per-session sender task (core 0) drains it with non-blocking socket writes. WiFi stalls no longer drop samples, and RTP frames are never cut mid-way.
- **Honest RTP timeline** — sequence and timestamp advance for every captured frame, so a rare drop reaches BirdNET-Go as a gap instead of a click.
- **Stream Health** — live queue depth, dropped frames, I2S DMA overruns and longest send stall in the Web UI.
- **mDNS discovery** — `atomecho.local`, no IP needed
- **Web UI** — configure settings, view signal levels, logs, and diagnostics
- **AGC** — automatic gain control with per-sample gain ramping (no zipper noise)
- **High-pass filter** — 2nd-order Butterworth (default 300Hz) removes wind/traffic
- **Thermal protection** — configurable auto-shutdown, trips only after 3 consecutive minutes over the limit
- **WiFi power save option** — modem sleep between beacons, the largest thermal lever on this board (needs an AP with DTIM period 1; auto-suspended if the stream drops frames)
- **LED indicator** — Off / Static / Level modes
- **WiFiManager** — captive portal for initial WiFi setup
- **Persistent settings** — saved to flash

## Quick Start

### 1. Flash
```bash
pio run --target upload
```

### 2. Connect to WiFi
On first boot, connect to the `ESP32-RTSP-Mic-AP` access point and configure your WiFi. The LED turns **blue** when ready.

### 3. Stream
```bash
vlc rtsp://atomecho.local:8554/audio
# or
ffplay -rtsp_transport tcp rtsp://atomecho.local:8554/audio
```

**BirdNET-Go**: set audio source to `rtsp://atomecho.local:8554/audio`

**Web UI**: `http://atomecho.local/`

## Recommended Settings

| Setting | Default | Notes |
|---------|---------|-------|
| Sample Rate | 16000 Hz | Optimal for PDM on Atom Echo |
| Gain | 3.0x | Good for outdoor use; changes apply live |
| AGC | OFF | Enable for varying bird distances |
| High-Pass | ON, 300 Hz | Removes rumble, keeps bird calls |
| Buffer | 1024 samples | 64 ms per packet; the send queue holds ~1.5 s regardless |
| CPU | 80 MHz | Enough for the 16 kHz pipeline; runs coolest. 160 MHz if Stream Health shows drops |
| WiFi Power Save | OFF | Turn ON to cut heat once the stream is stable; works only with AP DTIM period 1, otherwise it suspends itself after 10 dropped frames |
| I2S Shift | 0 bits | Fixed for PDM — do not change |

## Upgrading from 2.x

Flash 3.0.0 over any 2.x install; WiFi credentials and settings are kept. Stored settings win over the new defaults, so an upgraded device keeps CPU 160 MHz until you press **Defaults** or pick 80 MHz in the Web UI. A stored 120 MHz value (never a valid clock) is replaced by 80 MHz automatically.

The ESP32's internal temperature sensor is uncalibrated and typically reads 10–20 °C above the die. Treat the thermal readout as relative.

## LED Status

| Color | Meaning |
|-------|---------|
| Yellow | Starting up |
| Blue | Ready, waiting for connection |
| Green | Streaming |
| Orange | Signal hot, >70% (level mode) |
| Red | Clipping or thermal protection |

## Building

```bash
pio run                      # Build
pio run --target upload      # Flash
pio device monitor -b 115200 # Serial monitor
```

### Dependencies
```ini
lib_deps =
    tzapu/WiFiManager @ ^2.0.17
    m5stack/M5Atom @ ^0.1.3
    fastled/FastLED @ ^3.10.3
```

## Documentation

- [Architecture & Troubleshooting Guide](docs/DETAILS.md) — pipeline architecture, audio tuning, troubleshooting, version history

## Acknowledgments

This project is largely based on [birdnetgo-esp32-rtsp-mic](https://github.com/Sukecz/birdnetgo-esp32-rtsp-mic) by [@Sukecz](https://github.com/Sukecz) — thank you for the excellent foundation!

- M5Stack for the Atom Echo hardware
- [BirdNET-Go](https://github.com/tphakala/birdnet-go) community
