# Groove & Grill

A combined **Sonos music controller** and **BBQ temperature monitor** for the
kitchen/patio, running on an ESP32-S3 with a 480×480 round touch display.

Control your Sonos system — browse and play favourites, skip tracks, adjust
volume — and (in progress) keep an eye on up to four grills with live meat and
grill-temperature gauges, all from one round, always-on device.

> Firmware version: **0.1.0** · Status: **active development**

---

## Contents
- [Hardware](#hardware)
- [Software / build](#software--build)
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

**BBQ probes** (planned — see [Roadmap](#roadmap--to-do))
- *Wireless option:* off-the-shelf BLE BBQ thermometers (e.g. Wenmeice/Inkbird
  style), read passively from the ESP32-S3's built-in Bluetooth.
- *Wired satellite option:* a small ESP32-S3-Zero node with up to 4× MAX31855
  amplifiers reading **Type-K thermocouples**, relaying readings over BLE.
  Type-K is required for Big Green Egg-class grill-ambient temperatures
  (fibreglass/mineral-insulated leads for the high-heat grill probe; food-grade
  stainless probes for meat).

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
**WiFi · Speaker · OTA Update · Screensaver · About**

### Screensaver
After a period of inactivity the display shows a **clock + weather** widget;
touch to wake. Dimming and screensaver timers are configurable in Settings.

---

## BBQ monitoring

> The BBQ screens are complete; the probe **data source is currently mocked**
> until the BLE/thermocouple backend lands (see Roadmap). The "＋" and Bluetooth
> controls simulate connecting/disconnecting a probe so the UI states can be
> exercised.

- Supports up to **4 grills**; swipe left/right to move between grills and the
  **Add Grill** slot.
- Each grill shows **two concentric gauges** — outer = grill temperature,
  inner = meat temperature — filling toward their targets, with a centre meat
  icon and live/target readouts.
- **Config (gear):** set the **target grill temperature** (5 °C steps, applied
  live) and pick the meat — **Chicken / Lamb / Pork / Beef**. Chicken uses a
  single food-safe target; the others open a **doneness** slider
  (rare → well-done) whose targets come from `data/meat_temps.json`.
- **Bluetooth icon:** blue when a probe is connected, grey when not; a
  disconnected probe raises a flashing alarm.

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
  ui_bbq.c               grill carousel + gauges
  ui_bbq_config.c        grill config (target temp + meat select)
  ui_bbq_doneness.c      doneness selection
  ui_widgets.c           clock + weather screensaver
  bbq_controller.*       grill/probe data model (mock probe backend)
  sonos_controller.c     Sonos discovery, polling, playback, favourites
  ui_art.c               album-art download / decode / cache
  cst820.c / tca9554.c   touch + IO-expander drivers
  web_server.c           /setup web UI for favourites & WiFi
  img_*.c                generated image assets
data/meat_temps.json     meat doneness → target-temperature table (tracked)
scripts/                 codegen for meat data + icons
partitions.csv           16 MB flash layout (dual OTA + 2 MB SPIFFS art cache)
```

---

## Roadmap / to-do

- **BBQ probe backend:** replace the mock in `bbq_controller.c` with real
  readings — passive BLE scanning of wireless thermometers and/or the wired
  ESP32-S3-Zero + MAX31855 + Type-K satellite.
- **Now-playing progress:** parse and display track progress on the music arc.
- **Memory:** screens are cached for the session and never freed, so the LVGL
  object pool only grows. Free unused screens (wire up `ui_screen_invalidate()`)
  or move the pool to PSRAM before adding many more screens.
- General testing and defect fixing across all features.

---

## Credits

- Graphical assets by **AngryAngShanghai**.
- Built on an ESP32-S3 round-display platform, ESP-IDF, and LVGL.
