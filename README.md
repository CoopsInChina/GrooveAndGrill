# Groove & Grill

A combined **Sonos music controller** and **BBQ temperature monitor** for the
kitchen/patio, running on an ESP32-S3 with a 480×480 round touch display.

Control your Sonos system — browse and play favourites, skip tracks, adjust
volume — and keep an eye on multiple grills and cuts of meat with live gauges,
reading temperatures over Bluetooth from wired thermocouples and wireless probes
via a companion **BBQ Box** gateway.

> Firmware version: **0.1.0** · Status: **active development**

---

## Contents
- [Hardware](#hardware)
- [Software / build](#software--build)
- [Releasing & installing](#releasing--installing)
- [First-time setup](#first-time-setup)
- [Using the device](#using-the-device)
- [BBQ monitoring](#bbq-monitoring)
- [Project layout](#project-layout)
- [Roadmap / to-do](#roadmap--to-do)
- [Credits](#credits)

---

## Hardware

**Main unit**
- **MCU:** ESP32-S3 (dual-core Xtensa LX7 @ 240 MHz)
- **PSRAM:** 8 MB octal · **Flash:** 16 MB (dual OTA slots + 2 MB SPIFFS art cache)
- **Display:** 480×480 round RGB LCD
- **Touch:** CST820 capacitive controller (I²C)
- **IO expander:** TCA9554 (shares the I²C bus; drives display/touch reset lines)
- **Power:** mains-powered (USB-C)

**BBQ probes** — via the companion **[ESP BLE BBQ Box](https://github.com/CoopsInChina/ESP-BLE-BBQ-Box)** gateway
- The Box reads up to **4× Type-K thermocouples** (MAX31855) *and* connects to
  **wireless BLE probes**, then broadcasts everything in one connectionless BLE
  advertisement.
- This display is a passive **observer** of that advertisement — no pairing, no
  per-probe cost on the display. Type-K covers Big Green Egg-class grill-ambient
  temperatures (fibreglass/mineral-insulated leads); food-grade stainless or
  wireless probes suit the meat.
- *(Planned)* a standalone mode where the display connects **directly** to a
  single wireless probe without the Box (see [Roadmap](#roadmap--to-do)).

---

## Software / build

- **Framework:** ESP-IDF 5.3.1 via **PlatformIO** (`espressif32@6.9.0`)
- **UI:** LVGL 8.3
- **Music backend:** a [`node-sonos-http-api`](https://github.com/jishi/node-sonos-http-api)
  instance on your LAN (auto-discovered / cached), plus direct SSDP discovery of
  Sonos speakers, with album art fetched from the speaker.

```bash
# Build
pio run

# Flash + monitor
pio run -t upload -t monitor
```

Two files are intentionally **not** committed and are needed locally:
- `src/wifi_config.h` — WiFi credentials (git-ignored).
- `sdkconfig.music_meat` — generated per-machine from `sdkconfig.defaults`.

Generated sources (regenerate after editing their inputs):
- `python3 scripts/gen_meat_temps.py` → `src/meat_temps.h` (from `data/meat_temps.json`)
- `python3 scripts/gen_meat_icons.py` → `src/img_meat_icons.c` (from `assets/Meat Icons/`)

---

## Releasing & installing

**Only builds pushed to the `release` branch are ever published.** Merging
`dev` → `release` (or pushing directly) triggers
[`.github/workflows/release.yml`](.github/workflows/release.yml), which builds
the firmware and publishes it to **GitHub Pages**:

| What | Where |
|---|---|
| Browser-based USB flash (no software install — Chrome/Edge, Web Serial) | `https://coopsinchina.github.io/GrooveAndGrill/` |
| Version the device checks against (Settings → OTA Update) | `.../version.json` |
| The app image itself | `.../firmware.bin` |

One-time repo setup: **Settings → Pages → Source → "GitHub Actions"**.

**Bump the version** before releasing — it's the single source of truth read
by both the firmware build and the release workflow:
```ini
# platformio.ini
build_flags =
    -DFIRMWARE_VERSION=\"0.2.0\"
```

**Installing:**
- **First-time / bare board:** open the Pages URL above on Chrome/Edge, plug
  in via USB-C, click Install. Flashes bootloader + partition table + app.
- **Already running Groove & Grill:** **Settings → OTA Update** → *Check for
  Update* → *Update Now*. Downloads `firmware.bin` into the inactive OTA slot
  over Wi-Fi and reboots — no cable needed. If the new image crashes before
  finishing boot, the bootloader automatically rolls back to the previous
  slot (`CONFIG_APP_ROLLBACK_ENABLE`).

---

## First-time setup

1. **WiFi:** on boot, if no credentials are stored the device starts a setup
   access point (`MusicMeat-Setup`). Join it and follow the on-screen QR code /
   URL to enter your network details.
2. **Sonos:** the device discovers speakers via SSDP and locates a
   `node-sonos-http-api` server on your LAN automatically; the last-used speaker
   and API server are cached in NVS.
3. **Favourites:** browse to the device's **`http://<device-ip>/setup`** page
   (a QR code is shown on the Favourites "＋" slot) to add playlists/stations by
   Spotify link or URL and manage cached artwork.

The boot screen shows three status dots — **WiFi**, **Sonos**, **Server** —
each grey (checking) → green (OK) or red (failed).

---

## Using the device

Navigation is by **tap** and **swipe**, with a **home button** at the top of most
screens that returns to the main menu.

### Main menu
Tiles for **Music**, **BBQ**, and **Settings** (gear).

### Music screen
| Action | Result |
|---|---|
| Tap anywhere | Play / pause |
| Swipe up / down | Next / previous track |
| Swipe left | Go to Favourites |
| Volume button (bottom) | Open the Volume screen |
| Home button (top) | Return to menu |

Artist is shown above the album art, track title below it.

### Favourites
- **Swipe left / right** to browse your saved favourites and the "＋" (add) slot.
- **Tap** a favourite to play it.
- On the **＋ slot**, tap to show a QR code linking to the web setup page.
- Page dots at the bottom indicate position.

### Volume
- **Drag the arc** or **swipe up / down** to change volume (reads the speaker's
  actual level on entry).
- Auto-returns to the music screen after ~10 s of inactivity.

### Settings (swipe left / right through pages)
**WiFi · Speaker · BBQ Source · OTA Update · Screensaver · About**

OTA Update: shows the running version, *Check for Update* / *Update Now*
(manual only — see [Releasing & installing](#releasing--installing)). A
successful update hands off to a 5-second reboot countdown screen.

### Screensaver
After a period of inactivity the display shows a **clock + weather** widget;
touch to wake. Dimming and screensaver timers are configurable in Settings.

---

## BBQ monitoring

Temperatures arrive over BLE from the **BBQ Box** gateway (thermocouples +
wireless probes). The model is **sensor-centric**: every sensor is allocated to
a **grill number** and a **role** — grill-ambient or meat — with its own target.
Allocations are **saved to NVS**, so a cook survives a reboot. A grill can carry
several meats, each shown on its own screen.

**On the display**
- The BBQ carousel shows **one gauge screen per meat** (labelled with its grill
  number, sharing that grill's ambient reading), plus ambient-only screens and
  an **Add Meat** slot; swipe to move between them.
- Two concentric gauges — outer = grill temp, inner = meat temp — fill toward
  their targets, with a centre meat icon and live/target readouts ("– C" until
  the sensor reports).
- **Add Meat wizard:** pick a grill → pick a free sensor → pick its type (only
  asked when the grill has no ambient sensor yet) → set the target (grill-temp
  slider, or meat kind + doneness). Meats: **Chicken / Lamb / Pork / Beef** —
  chicken uses a single food-safe target; the rest use a **doneness** slider
  whose targets come from `data/meat_temps.json`.
- **⚙ Config** on any screen re-opens that sensor's settings; a **🗑 delete**
  frees it.
- The **Bluetooth icon** reflects the Box link — blue when the Box is being
  heard, dim when it's silent.

**On the web** — `http://<device-ip>/bbq`
- Live view **and** allocation for every sensor via **Grill # ▸ Type ▸ Meat ▸
  Target** dropdowns (meat targets use the same doneness table as the display).
- The web page and the on-screen config write the **same** NVS-backed model, so
  edits from either surface stay in sync.

---

## Project layout

```
src/
  main.c                 startup, task/UI bring-up
  ui_common.*            screen registry, navigation, gestures, home button
  ui_boot.c              boot / status screen
  ui_menu.c              main menu
  ui_sonos_main.c        now-playing / music screen
  ui_favourites.c        favourites carousel
  ui_volume.c            volume arc
  ui_settings.c          settings carousel (WiFi/Speaker/OTA/Screensaver/About)
  ui_bbq.c               cook-view carousel + gauges (one screen per meat)
  ui_bbq_add.c           "Add Meat" wizard (grill → sensor → type)
  ui_bbq_config.c        sensor config (grill target / meat select, delete)
  ui_bbq_doneness.c      doneness selection
  ui_widgets.c           clock + weather screensaver
  ui_reboot.*            shared "changing settings — rebooting in N" screen
  bbq_controller.*       sensor-centric BBQ model + NVS persistence
  bbq_ble.*              passive BLE observer of the BBQ Box advertisement
  ble_probe.*            direct multi-probe BLE central (standalone mode)
  ota_update.*           OTA check/download client (esp_https_ota)
  sonos_controller.c     Sonos discovery, polling, playback, favourites
  ui_art.c               album-art download / decode / cache
  cst820.c / tca9554.c   touch + IO-expander drivers
  buzzer.c               onboard buzzer (TCA9554 pin 8) — BBQ alarm
  web_server.c           /setup — tabbed Music Setup / BBQ Setup page
  img_*.c                generated image assets
data/meat_temps.json     meat doneness → target-temperature table (tracked)
scripts/                 codegen for meat data + icons
web/pages/index.html     GitHub Pages web-flash landing page (ESP Web Tools)
.github/workflows/       release.yml — builds `release` pushes, publishes Pages
partitions.csv           16 MB flash layout (dual OTA + 2 MB SPIFFS art cache)
```

---

## Roadmap / to-do

- **Standalone wireless probe:** let the display connect **directly** to a
  single BLE probe (no Box), with a **Settings page to switch** between
  "BBQ Box" and "Wireless Probe" source modes.
- **Web consolidation:** merge the Favourites setup (`/setup`) and BBQ sensor
  setup (`/bbq`) onto a single view.
- **Add Meat wizard polish:** tidy minor UI overlaps in the wizard step screens
  (low priority).
- **Box gateway bring-up:** finish wireless-probe support on the Box side
  (probes report as 0 until it connects/forwards them).
- **Cleanup:** prune the now-dead legacy grill shim in `bbq_controller.c`
  (`bbq_get_grill` / `bbq_set_targets` / `bbq_add_grill` / mock hooks).
- **Now-playing progress:** parse and display track progress on the music arc.
- **Memory:** screens are cached for the session and never freed, so the LVGL
  object pool only grows. Free unused screens (wire up `ui_screen_invalidate()`)
  or move the pool to PSRAM before adding many more screens.
- General testing and defect fixing across all features.

---

## Credits

- Graphical assets by **AngryAngShanghai**.
- Built on an ESP32-S3 round-display platform, ESP-IDF, and LVGL.
