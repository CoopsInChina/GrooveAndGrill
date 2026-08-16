# Groove & Grill

<img src="assets/ReadmePictures/BootScreen.jpg" alt="Groove & Grill boot screen" width="280">

A combined **Sonos music controller** and **BBQ temperature monitor** for the
kitchen/patio, running on an ESP32-S3 with a 480×480 round touch display.

Control your Sonos system — browse and play favourites, skip tracks, adjust
volume — and keep an eye on multiple grills and cuts of meat with live gauges,
reading temperatures over Bluetooth from wired thermocouples and wireless
probes, either via a companion **BBQ Box** gateway or connected directly to
the display in standalone mode.

> Firmware version: **1.1.0** · Status: **active development**

---

## Contents
- [Screenshots](#screenshots)
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

## Screenshots

<table>
<tr>
<td align="center"><img src="assets/ReadmePictures/MenuScreen.jpg" width="200"><br>Main menu</td>
<td align="center"><img src="assets/ReadmePictures/MainSonos.jpg" width="200"><br>Now playing</td>
<td align="center"><img src="assets/ReadmePictures/MeatSelection.jpg" width="200"><br>Meat picker</td>
<td align="center"><img src="assets/ReadmePictures/GrillScreen.jpg" width="200"><br>Live probe gauge</td>
</tr>
</table>

---

## Hardware

**Main unit** — [Waveshare ESP32-S3-Touch-LCD-2.1](assets/ReadmePictures/WaveshareAdvert.jpg)
(2.1″ round touch display board)

<img src="assets/ReadmePictures/WaveShareBack.jpg" alt="Waveshare ESP32-S3-Touch-LCD-2.1 board, back side" width="320">

- **MCU:** ESP32-S3 (dual-core Xtensa LX7 @ 240 MHz)
- **PSRAM:** 8 MB octal · **Flash:** 16 MB (dual OTA slots + 2 MB SPIFFS art cache)
- **Display:** 480×480 round RGB LCD
- **Touch:** CST820 capacitive controller (I²C)
- **IO expander:** TCA9554 (shares the I²C bus; drives display/touch reset lines,
  and the onboard **buzzer** used for the BBQ lost-connection alarm)
- **Power:** mains-powered (USB-C)

**BBQ probes** — two supported sources, switchable in **Settings → BBQ Source**
(see [BBQ monitoring](#bbq-monitoring)):
- **BBQ Box** (recommended for multiple grills/meats) — the companion
  **[ESP BLE BBQ Box](https://github.com/CoopsInChina/ESP-BLE-BBQ-Box)** reads up
  to **4× Type-K thermocouples** (MAX31855) *and* connects to wireless BLE
  probes, broadcasting everything in one connectionless BLE advertisement; the
  display just passively **observes** it — no pairing, no per-probe cost on the
  display. Type-K covers Big Green Egg-class grill-ambient temperatures
  (fibreglass/mineral-insulated leads); food-grade stainless or wireless probes
  suit the meat.
- **Standalone wireless probes** (no Box needed) — the display connects
  **directly** as a BLE central to up to 2 wireless meat probes (a BLE
  thermometer using a NOTIFY-characteristic protocol; see
  [`docs/bbq_ble_probe_protocol.md`](docs/bbq_ble_probe_protocol.md)). Good for
  a quick single- or dual-probe cook without setting up the Box.

---

## Software / build

- **Framework:** ESP-IDF 5.3.1 via **PlatformIO** (`espressif32@6.9.0`)
- **UI:** LVGL 8.3
- **Music backend:** direct SSDP discovery of Sonos speakers and **direct UPnP
  SOAP playback** (`AddURIToQueue`/`SetAVTransportURI`) — no external server
  needed for the common case. Album art is fetched straight from the speaker.
  An optional [`node-sonos-http-api`](https://github.com/jishi/node-sonos-http-api)
  instance on your LAN (auto-discovered/cached if present) is used only as a
  fallback for sources direct SOAP doesn't cover (e.g. Apple Music) and for
  browsing Sonos's own built-in Favorites.

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

**Verifying a release** — confirm the binaries CI published are genuinely
built from the commit `version.json` claims, not something stale or tampered:

1. `version.json` on Pages carries the exact commit: `{"version":"1.0.0",
   "file":"firmware.bin","commit":"<sha>"}`.
2. `sha256sums.txt` (same folder) has the published SHA-256 of each binary.
3. Builds are **reproducible** (`CONFIG_APP_REPRODUCIBLE_BUILD` — strips the
   compile timestamp, the only non-deterministic input for a fixed commit +
   pinned toolchain), so rebuilding that exact commit locally reproduces the
   same bytes:
   ```bash
   git checkout <sha>          # the commit named in version.json
   pio run -e music_meat
   shasum -a 256 .pio/build/music_meat/firmware.bin
   ```
   Compare against the matching line in `sha256sums.txt`. A match confirms
   CI built from that source with no tampering in between; a mismatch means
   either the toolchain drifted from the pinned `espressif32@6.9.0` or the
   published artifact doesn't match that commit.

---

## First-time setup

1. **WiFi:** on boot, if no credentials are stored the device starts a setup
   access point (`MusicMeat-Setup`). Join it and follow the on-screen QR code /
   URL to enter your network details.
2. **Sonos:** the device discovers speakers via SSDP; the last-used speaker is
   cached in NVS. If a `node-sonos-http-api` instance happens to be on your LAN
   it's auto-discovered and cached too, but it's optional — playback works
   without it.
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

The model is **sensor-centric**: every sensor (a thermocouple or a wireless
probe) is allocated to a **grill number** and a **role** — grill-ambient or
meat — with its own target. Allocations are **saved to NVS**, so a cook
survives a reboot. A grill can carry several meats, each shown on its own
screen.

**Source mode — Settings → BBQ Source**
- **BBQ Box** (default): passive BLE observer of the companion Box's
  advertisement — thermocouples + up to 5 wireless probes it relays.
- **Wireless Probe**: the display connects directly, as a BLE central, to up
  to 2 wireless probes — no Box needed.
- Switching source reboots the device (different BLE roles need a fresh
  radio init) — a countdown screen confirms it, then it comes back up
  scanning in the new mode.

**On the display**
- The BBQ carousel shows **one gauge screen per meat** (labelled by grill
  number in Box mode, or by probe slot — "Probe 1"/"Probe 2" — in standalone
  mode), plus ambient-only screens and an **Add Meat** slot; swipe to move
  between them.
- Two concentric gauges — outer = grill temp, inner = meat temp — fill toward
  their targets, with a centre meat icon and live/target readouts ("– C" until
  the sensor reports).
- **Add Meat wizard:** pick a sensor → (Box mode only) pick a grill and
  whether it's the grill-ambient or a meat probe → set the target (grill-temp
  slider, or meat kind + doneness). In standalone probe mode the grill/type
  steps are skipped — every probe is a meat. Meats: **Chicken / Lamb / Pork /
  Beef** — chicken uses a single food-safe target; the rest use a
  **doneness** slider whose targets come from `data/meat_temps.json`.
- **⚙ Config** on any screen re-opens that sensor's settings; a **🗑 delete**
  frees it. In standalone mode, a probe's slot (1/2) is bonded the first time
  it's seen and stays stable until you delete it — it won't shuffle around
  as probes connect/disconnect.
- **Lost-connection alarm:** if a sensor that was reporting stops (probe
  disconnects, box goes quiet), its ring flashes at the last known reading,
  the meat icon swaps to a warning icon, and the onboard **buzzer** sounds
  until it's resolved.
- The **Bluetooth icon** reflects the active link for that screen — blue when
  its sensor's source (the Box, or that specific probe) is being heard, dim
  when it's silent.

**On the web** — `http://<device-ip>/setup` (BBQ Setup tab)
- Live view **and** allocation for every sensor via **Grill # ▸ Type ▸ Meat ▸
  Target** dropdowns (meat targets use the same doneness table as the display).
- Shows the current source mode with a switch button (with a reboot warning).
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
docs/                    protocol notes (BLE probe/box, Sonos direct playback)
assets/ReadmePictures/   photos used in this README
partitions.csv           16 MB flash layout (dual OTA + 2 MB SPIFFS art cache)
```

---

## Roadmap / to-do

- **Box gateway wireless-probe bring-up:** the Box's own probe connect/forward
  path isn't finished — probes report as 0 through it. (Standalone mode,
  where the display connects directly, works today and doesn't depend on this.)
- **OTA throughput:** downloads work (resumable via HTTP Range, survives a
  stall/retry) but have been slow on at least one network path — worth
  re-testing outside that network, and considering an alternate host (e.g.
  GitHub Releases) if it's consistently slow rather than link-specific.
- **Add Meat wizard polish:** tidy minor UI overlaps in the wizard step screens
  (low priority).
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
