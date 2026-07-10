# ZephCore v1.16.5-zephyr

> [!IMPORTANT]
> ## Before you upgrade
>
> **ESP32-S3 / ESP32-C boards — one-time serial reflash required; WiFi-OTA cannot bridge this update.**
> This release moves the application to a new flash location (`0x10000`). A board on the old layout can't
> update to this build over WiFi-OTA or the browser flasher — **flash the `-merged.bin` once over USB/serial.**
> Affected boards: *Heltec V3 / V4 / V4.3, Station G2, Wireless Tracker / V2, XIAO ESP32-S3 / C3 / C6, LilyGo T-Lora C6.*
> Your identity, contacts, channels, prefs, and BLE bonds are preserved. After this reflash, OTA and
> app-updates work again.
>
> **nRF52, classic ESP32 (T-Beam / PICO-D4), STM32WL, and native Linux are unaffected** — upgrade as usual.
>
> **From v1.16.2 / v1.16.3 / v1.16.4** — clean flash, no re-bond, bonds and data survive.
>
> **From v1.16.1 or older** — flash it; on first boot it clears BLE bonds automatically (identity, contacts, channels, prefs preserved). Re-bond your phone/desktop once.
>
> **Coming from Arduino MeshCore** — flash it; auto-formats on first boot (new identity, clean storage).
>
> Take this with a grain of salt — try the formatters if anything anomalous happens with your node.

---

The headline of this release is **browser flashing via the Mesh America configurator** — and, in wiring
it up, we found and fixed a bug that was silently breaking **WiFi-OTA on every ESP32-S3 / ESP32-C board**.

## Highlights

### New: flash ZephCore from your browser — Mesh America configurator
ZephCore is now a listed provider in the **[Mesh America Device Configurator](https://apps.meshamerica.com)**.
Pick your board and flash directly — companion (BLE and USB) or repeater — no toolchain, no drag-and-drop.
Catalog and firmware publish automatically each release. Details in
**[PROVIDER_CATALOG.md](https://github.com/liquidraver/ZephCore/blob/master/PROVIDER_CATALOG.md)**.

### Fixed: ESP32 WiFi-OTA silently reverted — upload hit 100%, then booted the *old* firmware
On ESP32-S3 / ESP32-C boards, the app and MCUboot bootloader were using different flash partition tables,
so each OTA update landed at an address the bootloader never checked. Every upload "succeeded" and then
quietly did nothing. Both images now share one partition map, and OTA works correctly. (The XIAO C3/C6 and
T-Lora C6 used the stock layout and were never affected.)

### ESP32 app slot moved to `0x10000`
The ESP32 app slot moved from `0x20000` to `0x10000` — matching the Arduino / ESP-IDF offset. This is the
change that fixes OTA above, and it also enables the configurator's fast **app-only update** (rewrites just
the app, keeps your settings). Requires the one-time serial reflash (see *Before you upgrade*).

## Recommended upgrade checklist

1. **ESP32-S3 / ESP32-C boards:** flash `-merged.bin` once over USB/serial. WiFi-OTA can't cross this update; data survives.
2. **Everything else, from v1.16.2 / v1.16.3 / v1.16.4:** just flash — bonds and data survive.
3. **From v1.16.1 or older:** just flash — self-migrates on first boot; re-bond once.
4. **From Official MeshCore:** just flash — auto-formats on first boot.
5. **Anything odd?** Format first.
