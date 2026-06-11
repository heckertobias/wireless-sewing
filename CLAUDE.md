# wireless-sewing — Bernina B 500 WiFi-USB-Stick

WiFi-fähiger Ersatz für den USB-Stick einer Bernina B 500 Stickmaschine.
Stickdateien werden über ein Webinterface (SoftAP) auf eine microSD-Karte
hochgeladen/gelöscht; die Maschine sieht die Karte ganz normal als USB-MSC
(Mass Storage). Basis ist das Espressif-Beispiel
[`usb_msc_wireless_disk`](https://github.com/espressif/esp-iot-solution/tree/master/examples/usb/device/usb_msc_wireless_disk)
aus `esp-iot-solution`, angepasst auf die hier verwendete Hardware.

## Hardware

**Board:** ESP32-S3 KEY V1.0 USB-Dongle (ESP32-S3-WROOM-1, 8 MB Flash, PSRAM
vorhanden aber **nicht aktiviert**, siehe „Offene Punkte" unten). Schaltplan
v1.0g. Steckt direkt als USB-Stick im Bernina-USB-Port.

**Storage:** externe microSD-Karte über SDMMC/SDIO, 4-Bit, mit externen
10K-Pullups. **Pflicht: FAT32** (siehe Abschnitt „Bernina-Spezifika").

| Signal | GPIO | Kconfig |
|---|---|---|
| SD CLK | 36 | `SDCARD_SDIO_CLK_PIN` |
| SD CMD | 35 | `SDCARD_SDIO_CMD_PIN` |
| SD D0 | 37 | `SDCARD_SDIO_D0_PIN` |
| SD D1 | 38 | `SDCARD_SDIO_D1_PIN` |
| SD D2 | 33 | `SDCARD_SDIO_D2_PIN` |
| SD D3 | 34 | `SDCARD_SDIO_D3_PIN` |
| USB D-/D+ (native OTG) | 19 / 20 | ESP32-S3 fest verdrahtet, keine Config |
| UART0 Debug TX/RX (am `PROGRAM`-Header nachrüstbar) | 43 / 44 | nur für externen USB-TTL-Adapter |

Board-Auswahl in Kconfig: `DEVELOPMENT_BOARD_SELECTION` →
`ESP32_S3_GENERIC` (kein BSP, keine `esp32_s3_usb_otg`-Abhängigkeit).

**Wichtig:** Native USB-OTG (TinyUSB-MSC) und USB-Serial-JTAG teilen sich
denselben PHY an GPIO19/20 — nur eines kann gleichzeitig aktiv sein. Sobald
TinyUSB-MSC läuft, ist die USB-Konsole weg (siehe „Debugging" unten). Der
`PROGRAM`-Header (UART0/GPIO0/EN) ist auf diesem Board **unbestückt** — Flashen
läuft trotzdem über native USB, siehe nächster Abschnitt.

## Setup (ESP-IDF v6.0.1)

```bash
# Einmalig, falls die Python-venv fehlt (z.B. frischer Checkout):
~/.espressif/v6.0.1/esp-idf/install.sh esp32s3

# In jeder neuen Shell:
. ~/.espressif/v6.0.1/esp-idf/export.sh
```

Target ist bereits gesetzt (`sdkconfig` vorhanden, `IDF_TARGET=esp32s3`).
Bei Bedarf erneut: `idf.py set-target esp32s3` (lädt dabei auch die
Component-Manager-Abhängigkeiten neu auf, siehe „Vendorte Komponenten"
unten — `dependencies.lock` und `managed_components/` sollten dabei
unverändert bleiben, solange `main/idf_component.yml` nicht geändert wird).

## Build & Flash

```bash
. ~/.espressif/v6.0.1/esp-idf/export.sh
cd ~/Documents/repos/wireless-sewing
idf.py build
```

**Flash-Prozedur (BOOT-Download-Modus, kein UART-Adapter nötig):**
1. BOOT-Taste auf dem Dongle gedrückt halten.
2. USB-Kabel einstecken (Stick wird mit gehaltenem BOOT erkannt).
3. BOOT loslassen.
4. `idf.py -p /dev/cu.usbmodem1101 flash`

Der ROM-Bootloader bringt im Download-Modus USB-Serial-JTAG zurück, daher
funktioniert der normale Flash-Workflow unverändert. **Kein Auto-Reset**
nach dem Flashen zu erwarten, sobald die MSC-Firmware läuft — siehe
„Debugging".

## Architektur (Kurzfassung)

- `main/app_main.c`: `init_fat()` mountet die SD-Karte (SDMMC/4-Bit) unter
  `/disk` via `esp_vfs_fat_sdmmc_mount`. Danach `tinyusb_driver_install()`
  mit den rohen MSC-Callbacks (`tud_msc_read10_cb` / `tud_msc_write10_cb` /
  `tud_msc_capacity_cb` / `tud_msc_test_unit_ready_cb` / `tud_msc_inquiry_cb`
  / `tud_msc_start_stop_cb` …), die direkt auf `disk_read`/`disk_write`
  (`s_pdrv`) gehen — **keine** VFS-Indirektion auf dem USB-Pfad.
- `main/app_http_server.c`: `esp_http_server`-Instanz, die **dieselbe**
  FAT-Partition über VFS (`fopen`/`fwrite`/`unlink` unter `/disk`) bedient —
  Upload, Download, Liste, Delete, WiFi-Settings (`/settings.html`).
- `main/app_wifi.c`: SoftAP + optional STA-Join, Konfiguration in NVS
  persistiert, einstellbar über `/settings.html`.
- Gemeinsames Volume, zwei Schreibpfade (Web via VFS, USB-Host via raw
  Sektor-I/O) — siehe „Bernina-Spezifika" für die Synchronisierung.

## Bernina-Spezifika

### FAT32 ist Pflicht
Die Bernina B 500 erwartet FAT32 (kein exFAT, kein FAT16). Interner Flash
(≤ 7 MB nutzbar) ist für spec-konformes FAT32 zu klein (braucht ≥ 65525
Cluster) — daher microSD zwingend.

- `sdkconfig.defaults` aktiviert `FATFS_LFN_HEAP`, `FATFS_CODEPAGE_DYNAMIC`,
  `FATFS_API_ENCODING_UTF_8`, `FATFS_USE_LABEL`. exFAT bleibt **aus**
  (Default), `format_if_mount_failed` formatiert daher nie exFAT.
- **Karte vorformatieren** (am Mac), bevor sie zum ersten Mal eingesetzt
  wird:
  - Disk Utility → „MS-DOS (FAT)" für Karten ≤ 32 GB, oder
  - `diskutil eraseDisk FAT32 BERNINA MBRFormat /dev/diskN`
  - `format_if_mount_failed=y` ist nur ein Fallback und erzeugt ebenfalls
    nur FAT12/16/32 (nie exFAT), je nach Kartengröße.

### „Media changed" ohne Abziehen
Eine Stickmaschine enumeriert das MSC-Gerät einmal und liest die FAT danach
nicht erneut, auch wenn der Webserver Dateien hinzufügt/löscht. Lösung in
`main/app_http_server.c`:

- `msc_notify_media_change()`: pulst die native USB-OTG VBUS-valid-Leitung
  über die GPIO-Matrix kurz auf „invalid" (`usbd_vbus_enable(false)`),
  wartet `CONFIG_MSC_MEDIA_CHANGE_DELAY_MS` (Default 500 ms, Range
  50–5000 ms) und setzt sie wieder auf „valid". Der USB-Host (Bernina) sieht
  das als physisches Aus-/Einstecken und enumeriert das MSC-Gerät neu →
  liest die FAT neu ein.
- Wird automatisch aufgerufen:
  - nach erfolgreichem Datei-Upload (`upload_post_handler`, nach
    `FILE_WRITE_SUCCESS_BIT`)
  - nach erfolgreichem Datei-Löschen (`delete_post_handler`, nach
    `unlink()`)
- Manuell auslösbar über `GET /reset_msc` (z. B. zum Testen ohne
  Upload/Delete).
- Falls die Bernina das Timing nicht zuverlässig erkennt:
  `CONFIG_MSC_MEDIA_CHANGE_DELAY_MS` über `idf.py menuconfig` →
  „USB MSC Device Demo" → „USB media-change reconnect pulse duration (ms)"
  erhöhen.

### WiFi-Zugang
- SoftAP-SSID: `Bernina-Stick`, Passwort: `stickmaschine` (Defaults in
  `main/Kconfig.projbuild`, persistiert/änderbar via `/settings.html` und
  NVS — vor Inbetriebnahme ggf. eigenes Passwort vergeben).
- AP-IP: `192.168.4.1` (`SERVER_IP`).
- Optional: STA-Join ins Heimnetz über `ESP_WIFI_ROUTE_SSID`/`_PASSWORD`
  (leer = aus).

## Vendorte Komponenten (`components/esp_tinyusb`)

`espressif/esp_tinyusb` (aktuell `1.7.6~2`, neueste 1.x-Version im
Component-Registry) ruft in `tusb_msc_storage.c` noch die **alte**
4-Argument-Signatur von `esp_vfs_fat_register()` auf
(`esp_vfs_fat_register(base_path, drv, max_files, &fs)`). ESP-IDF **6.0**
hat diese Funktion auf eine 2-Argument-Signatur mit
`esp_vfs_fat_conf_t`-Struct umgestellt — die 1.x-Reihe von `esp_tinyusb`
wurde dafür nicht aktualisiert (die 2.x-Reihe enthält dafür einen großen,
inkompatiblen API-Refactor von `tusb_msc_storage.c` →
`tinyusb_msc.c`/`storage_sdmmc.c`/`storage_spiflash.c`, der weit über das
hinausgeht, was unser Code von der 1.x-API nutzt).

**Lösung:** `espressif/esp_tinyusb` 1.7.6~2 wurde nach
`components/esp_tinyusb/` vendort und in `tusb_msc_storage.c`
(`tinyusb_msc_storage_mount`) auf die neue
`esp_vfs_fat_conf_t`/2-Argument-Signatur gepatcht (1 Stelle, siehe
Kommentar im Code). `main/idf_component.yml` referenziert diese Kopie via
`override_path: "../components/esp_tinyusb"`.

- Wenn Espressif künftig eine 1.x-Version mit IDF-6.0-Fix veröffentlicht,
  kann `override_path` entfernt und die normale Registry-Version wieder
  verwendet werden.
- Ein Wechsel auf `esp_tinyusb` 2.x ist ein separates, größeres Vorhaben
  (API-Refactor in `app_main.c` nötig) und aktuell nicht geplant.

## Offene Punkte / Follow-ups

- **PSRAM nicht aktiviert.** Das Modul hat PSRAM, aber `sdkconfig.defaults`
  setzt bewusst kein `CONFIG_SPIRAM=y` und keinen expliziten Flash-Mode —
  ein falscher Quad-/Octal-Modus könnte den Boot brechen. Die App nutzt
  PSRAM nur optional für Diagnose-Logging (`#if CONFIG_SPIRAM`). Bei Bedarf
  später gezielt aktivieren und mit BOOT-Flash testen.
- `tud_msc_inquiry_cb` liefert noch generische Vendor/Product-Strings
  (`"ESP"` / `"Mass Storage"`) — rein kosmetisch, kein funktionaler
  Blocker.

## Debugging

Sobald TinyUSB-MSC läuft, ist die native-USB-Konsole weg (PHY ist mit der
Bernina/dem Host verbunden). Optionen für Laufzeit-Logs:
- UART0 (GPIO43 TX / GPIO44 RX, am `PROGRAM`-Header nachrüstbar) mit
  externem USB-TTL-Adapter + `idf.py monitor` (Mount-/SCSI-Logs etc.).
- Temporärer CDC+MSC-Composite-Debug-Build (nicht für den produktiven
  Bernina-Einsatz — produktiv bleibt MSC-only für maximale Kompatibilität).

## End-to-End-Verifikation

1. `idf.py build` fehlerfrei für `esp32s3`. ✅
2. Flashen via BOOT-Prozedur auf `/dev/cu.usbmodem1101`.
3. Stick am Mac einstecken → `diskutil info diskN` zeigt `FAT32`; Datei
   kopieren/lesen funktioniert.
4. Mit AP `Bernina-Stick` verbinden → `http://192.168.4.1` → Datei
   hochladen → **ohne Abziehen** erscheint sie am Mac-Mount nach
   ~`CONFIG_MSC_MEDIA_CHANGE_DELAY_MS` neu (Soft-Reconnect); Delete
   spiegelt sich analog.
5. Stick in die B 500 → vorhandene Stickdatei laden ✔; danach via Web neue
   Datei hochladen → Maschine zeigt die neue Datei **ohne
   Abziehen/Wiedereinstecken**; Delete-Test analog.
   `CONFIG_MSC_MEDIA_CHANGE_DELAY_MS` ggf. an das Bernina-Verhalten
   anpassen.
6. (Optional) UART0-Logs via USB-TTL-Adapter + `idf.py monitor`.
