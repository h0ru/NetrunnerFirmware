# Netrunner Firmware

## Prerequisites

1. **ESP-IDF v5.x** (tested on **v5.4.1**) installed, typically at `~/esp/esp-idf`.
   Follow Espressif's
   [Get Started guide](https://docs.espressif.com/projects/esp-idf/en/v5.4.1/esp32s3/get-started/index.html).
2. **cmake** and **ninja** on your `PATH` (installed by ESP-IDF's `install.sh`,
   or `sudo apt install cmake ninja-build`).
3. **Python 3.11-3.13** (ESP-IDF v5.4.1 is not validated on 3.14).
4. **Pillow** for the icon build step. Install it into the ESP-IDF virtualenv:
   ```bash
   source ~/esp/esp-idf/export.sh
   pip install -r requirements.txt
   ```

---

## Download

```bash
git clone https://github.com/h0ru/NetrunnerFirmware.git
cd NetrunnerFirmware
```

---

## Build & flash

### Option A: one command (recommended)

Connect the T-Embed over USB, then:

```bash
./buildAndFlash_T-Embed.sh
```

It sources ESP-IDF, sets the target on first run, builds, and flashes. Useful flags:

| Flag | Effect |
|---|---|
| `--port <device>` | Serial port (default: auto-detect `/dev/ttyACM*` on Linux, `/dev/cu.usbmodem*` on macOS) |
| `--monitor` | Open the serial monitor after flashing |
| `--build-only` | Build without flashing |

The ESP-IDF location can be overridden with `ESP_IDF_EXPORT_SCRIPT`, and the port
with the `ESPPORT` environment variable.

### Option B: manual (idf.py)

```bash
source ~/esp/esp-idf/export.sh
idf.py -B build_t_embed -DNETRUNNER_BOARD=lilygo_t_embed_cc1101 set-target esp32s3   # first time only
idf.py -B build_t_embed -DNETRUNNER_BOARD=lilygo_t_embed_cc1101 -p /dev/ttyACM0 build flash monitor
```

### Flash only (no rebuild)

If the firmware is already built in `build_t_embed/` and you just want to write
it again (e.g. flashing another unit):

```bash
source ~/esp/esp-idf/export.sh
cd build_t_embed
esptool.py --chip esp32s3 -p /dev/ttyACM0 -b 460800 \
    --before default_reset --after hard_reset write_flash @flash_args
```

---

## Troubleshooting

- **`"cmake" must be available on the PATH to use idf.py`**: cmake/ninja are not
  installed. Re-run `./install.sh esp32s3` inside your ESP-IDF folder (installs
  the bundled cmake/ninja) or `sudo apt install cmake ninja-build`, then
  `source ~/esp/esp-idf/export.sh` again.
- **`Could not open /dev/ttyACM0, the port is busy` / `Input/output error`
  (common on Kali/Debian)**: `ModemManager` grabs the serial port. Stop it and
  retry:
  ```bash
  sudo systemctl stop ModemManager      # or: sudo systemctl disable --now ModemManager
  sudo fuser -k /dev/ttyACM0            # free a port held by a stale monitor
  ```
  Also confirm your user is in the `dialout` group
  (`sudo usermod -aG dialout $USER`, then log out/in).
- **Port not found / device not responding**: check the real node with
  `ls /dev/ttyACM* /dev/ttyUSB*` and `dmesg | tail`. To force download mode:
  hold **BOOT (IO0)**, tap **RST**, release BOOT, then flash.
- **`ModuleNotFoundError: No module named 'PIL'`**: install Pillow
  (`pip install -r requirements.txt` inside the ESP-IDF venv).

---

## Credits & license

Built on upstream work:

- [Flipper Zero firmware](https://github.com/flipperdevices/flipperzero-firmware): the original firmware and app framework.
- [Flipper-Zero ESP32 Port](https://github.com/Sor3nt/Flipper-Zero-ESP32-Port): the ESP32 port this project is based on.
- [LilyGO T-Embed CC1101](https://lilygo.cc/en-us/products/t-embed-cc1101-plus): target hardware.

Distributed under the **GNU General Public License v3.0**. See
[`LICENSE`](LICENSE). Upstream components retain their original authors and
licenses.
