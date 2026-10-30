# CrowPanel 7" ESP32-P4: Upgrade C6 Co-processor Firmware via SDIO

Standalone ESP-IDF application that upgrades the ESP32-C6 co-processor on [Elecrow CrowPanel 7" ESP32-P4](https://www.elecrow.com/crowpanel-advanced-7-0-inch-esp32-p4-display.html) boards from factory firmware (esp\_hosted v2.3.0) to v2.9.7 — over the existing SDIO bus, without soldering.

## The Problem

The CrowPanel 7" ships with esp\_hosted **v2.3.0** on the ESP32-C6 WiFi co-processor. This version has SDIO transport bugs that cause WiFi to fail after about four minutes. Stable operation requires SDIO transport failure detection and automatic reinitialization, available in **v2.9.4 or later** on both host and slave.

USB updates reach the host (ESP32-P4) easily. The C6 has no exposed UART or USB port — its only external data connection is the SDIO bus shared with the P4. OTA through frameworks that start WiFi fails because WiFi triggers the transport failure before OTA completes.

**This tool runs OTA without WiFi.** A dedicated IDF application on the P4 initializes the SDIO transport, transfers the new C6 firmware from a LittleFS partition, and activates it. WiFi never starts, so the SDIO bus stays stable for the full transfer.

## How It Works

```
┌──────────────────┐    SDIO OTA RPC     ┌──────────────────┐
│    ESP32-P4       │───────────────────▶│    ESP32-C6       │
│   (this app)      │   1-bit, 10 MHz    │  (co-processor)   │
│                   │                    │                   │
│  LittleFS part.   │   1.5 KB chunks    │  Writes to        │
│  holds C6 binary  │───────────────────▶│  inactive OTA     │
│  (~1.1 MB)        │   ~15 seconds      │  partition         │
└──────────────────┘                    └──────────────────┘
```

1. Flash this app to the P4 (replaces your normal firmware temporarily)
2. App initializes SDIO transport — **no WiFi** (avoids bus contention with v2.3.0)
3. Reads C6 firmware binary from LittleFS partition on P4 flash
4. Transfers binary to C6 via esp\_hosted OTA RPC in 1.5 KB chunks
5. Activates new firmware on C6; C6 reboots into v2.9.7
6. Flash your normal firmware back to the P4

The C6 upgrade persists across P4 reflashes — the C6 has its own flash.

## Tested On

| Component | Version |
|-----------|---------|
| Board | CrowPanel Advanced 7" ESP32-P4 HMI (1024x600), PCB V1.0 |
| Factory C6 firmware | esp\_hosted v2.3.0 (internal Espressif staging build) |
| Target C6 firmware | esp\_hosted v2.9.7 |
| ESP-IDF | v5.5.1 |
| esp\_hosted component | v2.9.7 ([ESP Component Registry](https://components.espressif.com/components/espressif/esp_hosted/versions/2.9.7)) |

**Result:** Succeeded on first attempt, 2026-02-16. Zero solder. C6 upgraded from v2.3.0 to v2.9.7 in about 15 seconds.

## Prerequisites

### Hardware

- **CrowPanel 7" ESP32-P4** (PCB V1.0 tested) connected via USB-C cable to the **upper** USB-C port (CH341 UART bridge)
- **CH341 USB-UART driver** — Linux: built into kernel. macOS: [download from WCH](http://www.wch-ic.com/downloads/CH341SER_MAC_ZIP.html) if `ls /dev/cu.wchusbserial*` shows nothing when the board is connected.

### Software

- **ESP-IDF v5.5.1** — [installation guide](https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/get-started/index.html). Other v5.5.x versions may work but are untested. The installer handles Python, cmake, and esptool.py.
- **just** task runner — `brew install just` (macOS) or [other install methods](https://github.com/casey/just#installation)
- **curl** — for downloading the pre-built firmware binary. Standard on macOS and most Linux distributions.
- **git** — for ESP-IDF component manager and (optionally) building firmware from source.
- **Unix shell** — the justfile requires Bash and standard utilities (find, ls, wc). macOS and Linux work out of the box. Windows users need WSL.

> **Before every session:** source the ESP-IDF environment in your terminal:
> ```bash
> source $IDF_PATH/export.sh
> ```

### Firmware

- **C6 firmware binary** — `network_adapter.bin` for ESP32-C6, esp\_hosted v2.9.7 (see below)

### Obtaining the C6 Firmware Binary

**Option 1: Download pre-built binary (recommended)**

```bash
curl -L -o components/ota_littlefs/slave_fw_bin/network_adapter.bin \
    https://esphome.github.io/esp-hosted-firmware/v2.9.7/network_adapter_esp32c6.bin
```

Expected SHA256: `c9286c980b98b362c5b8862bf1d31bf6523e590cc68d70185b206d63f6c8bd11`

Verify after download:

```bash
shasum -a 256 components/ota_littlefs/slave_fw_bin/network_adapter.bin
```

**Option 2: Build from source**

```bash
git clone https://github.com/espressif/esp-hosted-mcu.git
cd esp-hosted-mcu/slave
idf.py set-target esp32c6
idf.py build
cp build/network_adapter.bin /path/to/c6-firmware-upgrade/components/ota_littlefs/slave_fw_bin/
```

**Verify the binary (either option):**

```bash
esptool.py image_info --version 2 components/ota_littlefs/slave_fw_bin/network_adapter.bin
```

Check:
- `Chip ID: 13 (ESP32-C6)` — correct target
- `Flash mode: DIO` — **avoid QIO** (QIO causes OTA failures on the C6)
- `App version:` — should show `2.9.7` (or whichever version you built)

## Quick Start

A `justfile` automates the full workflow. Verify your environment first:

```bash
just status                # verify serial port, IDF, firmware binary
```

### 1. Place the C6 firmware binary

```bash
# If you haven't downloaded it yet:
curl -L -o components/ota_littlefs/slave_fw_bin/network_adapter.bin \
    https://esphome.github.io/esp-hosted-firmware/v2.9.7/network_adapter_esp32c6.bin

just check-fw              # confirm binary found
just verify-fw             # inspect chip ID, flash mode, version
```

The build system automatically picks up any `.bin` file in this directory and packages it into the LittleFS partition image. Place **one** `.bin` file only.

### 2. Build

```bash
just set-target            # downloads managed components (esp_hosted v2.9.7, littlefs)
just build                 # checks firmware binary, then builds
```

> **Note:** The `ota_littlefs` component and `Kconfig.projbuild` are included in this repository. Do **not** run `just fix-components` — it overwrites these files with upstream defaults that lack project-specific configuration. The `fix-components` recipe exists only for development against a fresh esp\_hosted checkout without this repository's tracked components.

### 3. Backup (optional)

```bash
just backup                # reads 16 MB P4 flash to timestamped file
```

The backup is best-effort (esptool `read_flash` can be unreliable on CrowPanel). If you can rebuild your P4 firmware from source, you can skip this.

### 4. Flash and monitor

```bash
just flash                 # builds (if needed), then flashes to P4 and opens serial monitor
```

This **overwrites your current P4 firmware.** The OTA app runs once, upgrades the C6, then halts.

Alternatively, use `just flash-only` to flash without opening the monitor.

### 5. Read the serial output

Watch for these log tags:

| Tag | Meaning |
|-----|---------|
| `[PHASE]` | Progress through the five phases (0–5) |
| `[PASS]` | Success — phase completed |
| `[FAIL]` | Failure — includes error code and diagnostic hints |
| `[DIAG]` | Timing data, versions, transfer sizes |

**Success looks like this:**

```
I c6-sdio-ota: [PASS] Connected to C6 slave in 1884ms
I c6-sdio-ota: [DIAG] C6 firmware: 2.3.0
I c6-sdio-ota: [PASS] OTA transfer completed in 14832ms
I c6-sdio-ota: [PASS] Activate succeeded
I c6-sdio-ota: [DIAG] C6 version after OTA: 2.9.7
I c6-sdio-ota: [PASS] *** C6 UPGRADED TO v2.9.7 — SUCCESS ***
```

If you see `[PASS] C6 already at v2.9.7 — OTA not needed`, the C6 was already upgraded. Nothing more to do.

### 6. Restore your normal firmware

Flash your regular firmware back to the P4. The C6 retains v2.9.7 independently.

If you use **ESPHome**: reflash your ESPHome firmware via USB. No configuration changes needed — ESPHome detects the new C6 version automatically during boot.

If you use **plain ESP-IDF**: rebuild and flash your application with `idf.py flash`.

If you need to restore **Elecrow factory firmware**: download it from the [Elecrow CrowPanel GitHub repo](https://github.com/Elecrow-RD/CrowPanel-Advanced-7inch-ESP32-P4-HMI-AI-Display-1024x600-IPS-Touch-Screen) and flash with esptool.

### 7. Verify the upgrade

After your normal firmware boots, check the serial output for the esp\_hosted version handshake. Look for a line containing the C6 firmware version:

```
I esp_hosted: Slave chip_id [13] ESP32-C6
I esp_hosted: Slave FW version [2.9.7]
```

**WiFi stability test:** Connect to your network and confirm WiFi remains connected beyond four minutes. The v2.3.0 failure mode was a hard disconnect at ~4 minutes with no recovery. With v2.9.7, WiFi should remain connected indefinitely.

## Build Troubleshooting

### `ota_littlefs.h` not found

The `ota_littlefs` component lives inside the esp\_hosted example directory, not as a standalone component. Run:

```bash
just fix-components
just build
```

Or manually copy from the downloaded managed component:

```bash
HOSTED=$(find managed_components -name "espressif__esp_hosted" -type d | head -1)
cp -r "$HOSTED/examples/host_performs_slave_ota/components/ota_littlefs" components/
cp "$HOSTED/examples/host_performs_slave_ota/main/Kconfig.projbuild" main/
idf.py build
```

### `esp_app_desc.h` not found

Add `esp_app_format` to `PRIV_REQUIRES` in `main/CMakeLists.txt`:

```cmake
PRIV_REQUIRES esp_app_format ota_littlefs
```

### API function not found

The esp\_hosted public API changes between versions. After `idf.py set-target`, verify the expected functions exist:

```bash
grep -r "esp_hosted_connect_to_slave\|esp_hosted_slave_ota_begin" managed_components/
```

If a function is missing, check the actual header names and update `main.c`.

## Interpreting Failures

| Serial output | Cause | Next step |
|---|---|---|
| `[FAIL] esp_hosted_init` | SDIO transport broken at init | Verify serial port, check that P4 GPIO32 reaches C6 EN pin |
| `[FAIL] esp_hosted_connect_to_slave` | C6 not responding | Check C6 power (P37 test pad should read 3.3V) |
| `[FAIL] OTA transfer failed` + duration < 5s | Transport or OTA begin failed | SDIO fundamentally broken — skip to UART flash (see [Alternative Paths](#alternative-flash-paths)) |
| `[FAIL] OTA transfer failed` + duration 5–30s | Transport died mid-transfer | Try lower SDIO clock or smaller chunks (see [Tuning](#tuning-parameters)) |
| `[FAIL] OTA transfer failed` + duration > 30s | Timeout | C6 hung — power-cycle and retry |
| `[FAIL] Activate failed` | OTA wrote but could not mark bootable | Retry from scratch; C6 still on old firmware (safe) |

## Tuning Parameters

If OTA fails mid-transfer, try these adjustments one at a time:

| Parameter | Default | Try | File |
|-----------|---------|-----|------|
| SDIO clock | 10 MHz | 5 MHz | `sdkconfig.defaults` → `CONFIG_ESP_HOSTED_SDIO_CLOCK_FREQ_KHZ=5000` |
| Chunk size | 1500 B | 1400, 512, 256 | `components/ota_littlefs/ota_littlefs.c` → `CHUNK_SIZE` |
| Bus width | 1-bit | 4-bit | `sdkconfig.defaults` → remove `CONFIG_ESP_HOSTED_SDIO_1_BIT_BUS`, set `BUS_WIDTH=4` |

Rebuild with `idf.py build` after each change.

## Other Boards

The default configuration (`sdkconfig.defaults`) is the CrowPanel 7" reference. A different board that wires the C6 the
same way but resets it from another GPIO can use an overlay file, layered on top with `SDKCONFIG_DEFAULTS`:

| Board | Overlay | C6 reset | Notes |
|-------|---------|----------|-------|
| CrowPanel 7" ESP32-P4 | none (`sdkconfig.defaults`) | GPIO32 | reference design, tested |
| JCZN JC4880P443C_I_W | `sdkconfig.defaults.jczn` | GPIO54 | pin taken from the factory firmware boot log; reset polarity (active high) not yet tested on this board |

```bash
rm -f sdkconfig        # a sdkconfig generated for another board would keep its old values
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.jczn" set-target esp32p4
idf.py build
```

The `[DIAG]` line printed when `esp_hosted_init` fails shows the reset GPIO the build actually uses.

## Alternative Flash Paths

If SDIO OTA fails entirely, the C6 can still be flashed through test pads on the PCB. See [HARDWARE.md](HARDWARE.md) for test pad locations and wiring.

### UART flash (requires soldering 3 wires)

| Test Pad | C6 Pin | Connect to |
|----------|--------|------------|
| P25 | GPIO16 (TX) | USB-UART adapter RX |
| P21 | GPIO17 (RX) | USB-UART adapter TX |
| P36 | GPIO9 (BOOT) | GND during reset |

Hold P36 to GND, reset the C6 (power-cycle or toggle GPIO32), then flash:

```bash
esptool.py --chip esp32c6 --port /dev/ttyUSB0 --baud 115200 \
    write_flash --flash_mode dio \
    0x0     bootloader.bin \
    0x8000  partition-table.bin \
    0x10000 network_adapter.bin
```

> **Note:** The ESP32-C6 bootloader address is **0x0** (RISC-V architecture), not 0x1000 as on Xtensa-based ESP32 chips. Using the wrong address bricks the bootloader — repeat the UART flash to recover.

The UART approach requires `bootloader.bin` and `partition-table.bin` in addition to the app binary. Build them from source:

```bash
git clone https://github.com/espressif/esp-hosted-mcu.git
cd esp-hosted-mcu/slave
idf.py set-target esp32c6
idf.py build
# Binaries are at:
#   build/bootloader/bootloader.bin
#   build/partition_table/partition-table.bin
#   build/network_adapter.bin
```

## Safety

**P4 (main processor):** You cannot brick it. The CH341 USB-UART bridge connects permanently. `esptool.py` can always reflash the P4.

**C6 (co-processor):** OTA writes to the **inactive** partition slot only. A failed or interrupted transfer leaves the active slot (current firmware) intact. The only bricking scenario is a binary that passes validation but crashes at runtime — and even then, [UART flash](#alternative-flash-paths) recovers it.

Running this tool a second time is safe. If the C6 already runs v2.9.7, the app detects it and halts without writing anything.

## Hardware Reference

| Parameter | Value |
|-----------|-------|
| SDIO bus | 1-bit mode, GPIO14 (D0) |
| SDIO clock | 10 MHz (configurable) |
| SDIO CLK | P4 GPIO18 → C6 IO19 |
| SDIO CMD | P4 GPIO19 → C6 IO18 |
| C6 reset | P4 GPIO32 → C6 EN (active high) |
| C6 flash | 4 MB, dual OTA partitions (1920 KB each) |
| Serial port | CH341 UART, upper USB-C |

Series resistors with pullups to 3.3V terminate all six SDIO lines (D0–D3, CLK, CMD). Four-bit SDIO mode works electrically but remains untested for OTA.

For the full hardware analysis — pin map, test pad locations, connector inventory — see [HARDWARE.md](HARDWARE.md).

## P4 Flash Layout

This project uses a simplified partition table on the P4 (the OTA app does not need dual OTA):

```
Offset     Name         Size           Purpose
0x009000   nvs           16 KB         Non-volatile storage
0x00D000   otadata        8 KB         OTA state
0x00F000   phy_init       4 KB         PHY calibration
0x010000   ota_0          3 MiB        This application
0x310000   storage      1920 KB        LittleFS — holds the C6 firmware binary (~1.1 MB)
```

The build system automatically generates the LittleFS image from `components/ota_littlefs/slave_fw_bin/` and flashes it to the `storage` partition.

## Project Structure

```
├── justfile                            Task runner (just build, just flash, ...)
├── CMakeLists.txt                      Root build definition
├── sdkconfig.defaults                  CrowPanel SDIO config, OTA settings
├── partitions.csv                      P4 flash layout (app + LittleFS)
├── HARDWARE.md                         C6 pin map, test pads, SDIO bus details
├── main/
│   ├── main.c                          5-phase OTA workflow
│   ├── idf_component.yml               esp_hosted ==2.9.7, littlefs
│   └── Kconfig.projbuild               OTA method selection menu
└── components/
    ├── ota_littlefs/
    │   ├── ota_littlefs.c              LittleFS mount, binary validation, chunked transfer
    │   ├── ota_littlefs.h              Public API
    │   └── slave_fw_bin/               Place C6 .bin file here before build
    └── common_ota_scripts/
        ├── find_newest_firmware.cmake  Selects newest .bin at build time
        └── flash_selected_firmware.cmake
```

## Platform Notes

### macOS

Serial port auto-detection (`/dev/cu.wchusbserial*`) works out of the box.

### Linux

The justfile auto-detects macOS serial ports only. Override the port manually:

```bash
just flash serial_port=/dev/ttyUSB0
```

Or edit line 8 of the `justfile` to match your system:

```
serial_port := `ls /dev/ttyUSB* 2>/dev/null | head -1 || echo ""`
```

CH341 drivers are built into most Linux kernels. The device appears as `/dev/ttyUSB0`.

## Known Limitations

- **Tested on PCB V1.0 only.** Other CrowPanel revisions may have different GPIO assignments or C6 module variants.
- **Target firmware: v2.9.7 only.** Other esp\_hosted versions are untested. The download URL and SHA256 above are specific to v2.9.7.
- **4-bit SDIO mode untested.** The wiring supports it, but OTA reliability in 4-bit mode is unverified.
- **Version query may fail with v2.3.0.** The factory C6 firmware often times out on RPC version queries. The OTA proceeds anyway (version checks are disabled by default).
- **LittleFS partition is overwritten** when you flash new firmware to the P4. The C6 binary must be re-embedded each time you rebuild this app.
- **No automatic rollback on C6.** If the new firmware crashes, the C6 enters a reboot loop. Recovery requires running this tool again with a known-good binary, or UART flash via test pads.
- **`just fix-components` overwrites local edits.** It copies `ota_littlefs` and `Kconfig.projbuild` from the managed esp\_hosted component, replacing any local customizations. Back up modified files before running it.

## FAQ

**Will this brick my board?**
No. The P4 can always be reflashed via USB. The C6 OTA writes to the inactive partition — a failed transfer leaves the current firmware intact. See [Safety](#safety).

**Do I need to solder anything?**
No. The SDIO OTA path requires zero soldering. Soldering is only needed for the [UART fallback](#alternative-flash-paths) if SDIO OTA fails entirely.

**Can I run this tool twice?**
Yes. If the C6 already runs v2.9.7, the app detects it and halts without writing. Running it again is safe.

**Can I upgrade to a different version (not v2.9.7)?**
In principle, yes — place any esp\_hosted `network_adapter.bin` for ESP32-C6 in `slave_fw_bin/`. Verify it with `esptool.py image_info` first. Only v2.9.7 has been tested.

**What if I don't have the `just` tool?**
Run the ESP-IDF commands directly:

```bash
# Download C6 firmware (if not already present)
curl -L -o components/ota_littlefs/slave_fw_bin/network_adapter.bin \
    https://esphome.github.io/esp-hosted-firmware/v2.9.7/network_adapter_esp32c6.bin

# Build and flash
idf.py set-target esp32p4
idf.py build
idf.py -p /dev/cu.wchusbserial110 flash monitor
```

## Related Resources

- [Elecrow CrowPanel P4 repo](https://github.com/Elecrow-RD/CrowPanel-Advanced-7inch-ESP32-P4-HMI-AI-Display-1024x600-IPS-Touch-Screen) — schematics, factory code
- [Elecrow issue #5](https://github.com/Elecrow-RD/CrowPanel-Advanced-7inch-ESP32-P4-HMI-AI-Display-1024x600-IPS-Touch-Screen/issues/5) — community discussion on C6 flash access
- [esp-hosted-mcu](https://github.com/espressif/esp-hosted-mcu) — slave firmware source, OTA examples
- [esp\_hosted v2.9.7 on Component Registry](https://components.espressif.com/components/espressif/esp_hosted/versions/2.9.7) — managed component used by this project
- [tymorton/esp32-p4-c6-espnow-enabler](https://github.com/tymorton/esp32-p4-c6-espnow-enabler) — independent C6 OTA tool (v2.6.7 target, LittleFS method)
- [esp-serial-flasher](https://github.com/espressif/esp-serial-flasher) — alternative: SDIO download mode flash (requires 1 wire to GPIO9 test pad)

## License

Based on Espressif's `host_performs_slave_ota` example. [Apache-2.0](LICENSE).
