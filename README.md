# wireless-sewing

A WiFi-enabled USB-stick replacement for a Bernina B 500 embroidery machine,
built on an ESP32-S3.

## What it does

Embroidery files are uploaded and deleted through a small web interface
served from a SoftAP, and stored on a microSD card. The embroidery machine
sees that same microSD card as a regular USB mass-storage device (USB MSC) —
no extra drivers, no special software on the machine side.

When a file is added or removed via the web interface, the device briefly
toggles its USB connection ("media changed") so the machine re-reads the
file list **without needing to be unplugged and replugged**.

## Hardware

- **Board:** ESP32-S3 KEY V1.0 USB dongle (ESP32-S3-WROOM-1, 8 MB flash).
  Plugs directly into the Bernina's USB port.
- **Storage:** external microSD card via SDMMC/SDIO (4-bit), **must be
  formatted as FAT32** (required by the Bernina B 500).

| Signal      | GPIO |
|-------------|------|
| SD CLK      | 36   |
| SD CMD      | 35   |
| SD D0       | 37   |
| SD D1       | 38   |
| SD D2       | 33   |
| SD D3       | 34   |
| USB D-/D+   | 19 / 20 (native OTG, fixed) |
| UART0 debug TX/RX | 43 / 44 (optional, via external USB-TTL adapter) |

## Build & flash

Requires ESP-IDF v6.0.1.

```bash
. ~/.espressif/v6.0.1/esp-idf/export.sh
idf.py build
```

To flash, put the board into BOOT/download mode (hold BOOT, plug in USB,
release BOOT), then:

```bash
idf.py -p /dev/cu.usbmodem1101 flash
```

## Usage

1. Connect to the device's WiFi access point `Bernina-Stick`
   (default password `stickmaschine` — change this via the web UI before
   real use).
2. Open `http://192.168.4.1` in a browser to upload, list, and delete
   embroidery files on the microSD card.
3. Plug the device into the Bernina B 500 like a regular USB stick.

See [`CLAUDE.md`](CLAUDE.md) for full architecture, hardware, and
development details.

## License & attribution

This project is licensed under the **Apache License, Version 2.0** (see
[`LICENSE`](LICENSE)).

It is derived from Espressif's `usb_msc_wireless_disk` example
([esp-iot-solution](https://github.com/espressif/esp-iot-solution)) and
includes a vendored, locally patched copy of
[`esp_tinyusb`](https://github.com/espressif/idf-extra-components/tree/master/esp_tinyusb)
in `components/esp_tinyusb/`, both also Apache-2.0. See
[`NOTICE`](NOTICE) for attribution details and a summary of the changes
made.
