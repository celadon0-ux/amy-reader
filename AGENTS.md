# Codex Project Instructions

## Amy Reader Windows Build and X3 Flash Guide

Follow this guide whenever building or flashing Amy Reader from Windows.

### Safety and scope

- Preserve existing working-tree changes. Do not stage, commit, push, or discard files unless the user explicitly asks.
- Use only the repository-local tooling under `C:\amy-reader\.tools`. Do not install build dependencies globally.
- Use COM3 only for the X3. If COM3 is absent or does not identify as an ESP32-C3, stop after building; do not choose another port.
- Always read the connected device's current OTA metadata immediately before flashing. Never rely on an older dump or a previously active slot.
- Never write or erase `otadata` unless the user explicitly authorizes it.

### Build environment

Run PlatformIO through `cmd.exe` with UTF-8 enabled. Use quoted `set "NAME=value"` syntax. This avoids Windows `UnicodeEncodeError` failures and ensures all caches stay inside the repository.

```bat
cd /d C:\amy-reader
chcp 65001>nul
set "PYTHONUTF8=1"
set "PYTHONIOENCODING=utf-8"
set "TMP=C:\amy-reader\.tools\tmp"
set "TEMP=C:\amy-reader\.tools\tmp"
set "UV_CACHE_DIR=C:\amy-reader\.tools\uv-cache"
set "XDG_CACHE_HOME=C:\amy-reader\.tools\xdg-cache"
set "PIP_CACHE_DIR=C:\amy-reader\.tools\pip-cache"
set "PLATFORMIO_HOME_DIR=C:\amy-reader\.tools\platformio"
set "PLATFORMIO_CORE_DIR=C:\amy-reader\.tools\platformio"
set "PLATFORMIO_GLOBALLIB_DIR=C:\amy-reader\.tools\platformio\lib"
set "PLATFORMIO_SETTING_ENABLE_TELEMETRY=No"
C:\amy-reader\.tools\pio-venv\Scripts\python.exe -m platformio run -e default
```

Use `C:\amy-reader\.tools\pio-venv\Scripts\python.exe` for PlatformIO builds. Do not invoke PlatformIO or Python from the user's profile path; spaces in that path can break the custom Arduino-core builder.

After building, verify:

- `C:\amy-reader\.pio\build\default\firmware.bin` exists.
- Its first byte is ESP image magic `0xE9`.
- Its size does not exceed the `0x640000` application-partition size in `partitions.csv`.

### Identify the connected X3

Use the PlatformIO-managed Python for esptool:

```bat
C:\amy-reader\.tools\platformio\penv\Scripts\python.exe -m esptool --chip esp32c3 --port COM3 chip-id
```

Require a successful ESP32-C3 identification before proceeding.

### Read and select the active OTA slot

Create a new timestamped backup under `C:\amy-reader\.tools\tmp` on every flash attempt:

```bat
C:\amy-reader\.tools\platformio\penv\Scripts\python.exe -m esptool --chip esp32c3 --port COM3 --baud 921600 read-flash 0xe000 0x2000 C:\amy-reader\.tools\tmp\otadata-YYYYMMDD-HHMMSS.bin
```

Decode the two 32-byte OTA selection entries at file offsets `0x0000` and `0x1000`:

- OTA sequence: little-endian uint32 at entry offset `+0`.
- State: little-endian uint32 at `+24`.
- Sequence CRC: little-endian uint32 at `+28`.
- Reject sequence `0xFFFFFFFF`, invalid sequence CRC, state `INVALID` (`3`), and state `ABORTED` (`4`).
- Select the valid entry with the unique highest sequence. If no valid entry exists or the highest sequence is ambiguous, do not flash.
- Map `(ota_seq - 1) % 2` to the destination:
  - `ota_0/app0` -> `0x10000`
  - `ota_1/app1` -> `0x650000`
- Confirm the selected address and `0x640000` size agree with `partitions.csv` before flashing.

### Flash the verified active slot

Substitute only the address selected from the fresh OTA dump:

```bat
C:\amy-reader\.tools\platformio\penv\Scripts\python.exe -m esptool --chip esp32c3 --port COM3 --baud 921600 write-flash VERIFIED_ADDRESS C:\amy-reader\.pio\build\default\firmware.bin
```

Require esptool's data-hash verification, successful hard reset, and COM3 re-enumeration. Save and report the fresh OTA backup path, selected slot/address, firmware size/hash, and flash result.

If serial logs show a version such as `1.5.0-dev-branch-<hash>`, remember that the hash is derived from `HEAD` only and does not prove the working tree was clean.
