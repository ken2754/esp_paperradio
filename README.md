# esp_paperradio

Internet radio receiver firmware targeting the **Waveshare ESP32-S3 e-Paper 3.97"** board (ESP32-S3-WROOM-1-N16R8, SSD1677 800×480 e-Paper, ES8311 I2S codec + NS4150B mono amplifier, AXP2101 PMIC).

## Features
- **SomaFM Presets & Custom Stations**: Preloaded with SomaFM channels (Groove Salad, Drone Zone, Secret Agent, Lush, etc.), fully reconfigurable.
- **Embedded Web Configuration Portal (`http://paperradio.local`)**:
  - Add, edit, reorder, delete stations, control playback and volume from any browser on the local network.
  - **Multi-SSID Wi-Fi Profiles (Up to 5)**: Store up to 5 Wi-Fi credentials; automatically scans and connects to the AP with the strongest signal (highest RSSI).
  - **Safe Setup SoftAP (`PaperRadio-Setup`, `http://192.168.4.1`)**: Automatically launches if no configured networks are reachable or when holding `BOOT` for 3 seconds. The audio decoder and NS4150B amplifier remain safely powered down and muted while offline.
- **PSRAM Ring Buffer + Helix MP3 Decoding**:
  - 384 KB ring buffer in Octal PSRAM to prevent audio underruns during e-Paper refresh and network jitter.
  - Fixed-point Helix MP3 decoder running on a dedicated FreeRTOS task on Core 1.
- **Real-Time ICY Metadata & Album Artwork Display**:
  - Extracts `StreamTitle` (`Artist - Title`) from stream metadata in real-time.
  - Asynchronously queries the iTunes Search API and downloads `144x144` baseline JPEG cover art.
  - Decompresses JPEG using ESP32-S3 ROM TJpgDec, applies dynamic contrast stretching and 4-level Floyd-Steinberg error-diffusion dithering.
- **Hybrid 4-Gray & Zero-Flicker Partial Refresh**:
  - Uses full 4-grayscale refresh (`0xD7`) when new album art arrives or on full sync.
  - Uses differential partial refresh (`0xFF`, ~0.5s) for volume changes, station browsing, and track text updates without flickering or erasing on-screen 4-gray artwork.
- **High-Contrast 1bpp UI**:
  - High-contrast black-and-white layout for text, station presets, volume indicators, and status icons to ensure crisp differential updates.

## Hardware Controls
| State | Action | Function |
|---|---|---|
| **Playing** (`PLAYING` / `BUFFERING`) | Rotary `UP` | Volume +5% |
| **Playing** (`PLAYING` / `BUFFERING`) | Rotary `DOWN` | Volume -5% |
| **Playing** (`PLAYING` / `BUFFERING`) | Rotary `PUSH` | Stop (`STOP`) |
| **Stopped** (`STOPPED`) | Rotary `UP` | Next Station (`CH +1`) |
| **Stopped** (`STOPPED`) | Rotary `DOWN` | Previous Station (`CH -1`) |
| **Stopped** (`STOPPED`) | Rotary `PUSH` | Play selected station (`PLAY`) |
| Any | `BOOT` short press | Full EPD screen refresh |
| Any | `BOOT` long press (3s) | Enter Setup SoftAP mode |
| Any | `PWR` short press | Enter / exit sleep mode |
| Any | `PWR` long press (4s) | Hardware power off (AXP2101) |

## Build and Flash (ESP-IDF v5.3+)
```bash
# Source ESP-IDF environment
source ~/esp/esp-idf/export.sh

# Build project
idf.py build

# Flash and monitor
idf.py -p /dev/ttyACM0 flash monitor
```
