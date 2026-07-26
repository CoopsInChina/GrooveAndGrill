# Direct BLE meat-probe integration

Reads a wireless BLE BBQ thermometer directly (no external app), for meat /
lower-temp use alongside the wired [BBQ Box](https://github.com/CoopsInChina/ESP-BLE-BBQ-Box)
(high-heat grill ambient).

Protocol reference: **[bbq_ble_protocol.md](bbq_ble_protocol.md)** (reverse-engineered).

## The device is connection-based, not broadcast

Unlike the BBQ Box (connectionless advertising), this thermometer streams
temperature over **GATT notifications** — so this is a BLE **central/client**:

1. Scan for advertised name **`BBQ`**.
2. Connect.
3. Discover characteristic **`772ae377-b3d2-ff8e-1042-5481d1e03456`** (NOTIFY).
4. Enable notifications (write its CCCD).
5. Parse each notification: `[1 prefix byte][ASCII decimal temp]`, e.g.
   `@31.4` → 31.4 °C. **Single** temperature channel.

Reconnects automatically on disconnect.

## What's here

- **`ble_probe.c/.h`** — NimBLE central implementing the flow above. Exposes
  `ble_probe_get(&temp_c)` / `ble_probe_connected()` / `ble_probe_age_ms()`.
  Wired into `main.c` (`ble_probe_init()`); `sdkconfig.defaults` enables the
  NimBLE **central** role.

## Known-unknowns carried over from the protocol doc (⚠️)

These are unverified — surfaced, not coded around:

- **Prefix byte** meaning is unresolved (varies per session; not channel /
  checksum / counter). We keep it raw (`ble_probe_t.prefix`) and never act on it.
- **Temperature field width** past 100 °C, below 10 °C, or negative is untested
  (every capture was two-digit). Our parse is width-agnostic (`strtof` on the
  ASCII tail), which should handle these, but it's unproven on-device.
- Device Information Service (`0x180A`) strings not yet read (could ID a
  rebadged ODM board).

## To-do

- [ ] Verify on hardware: `ble_probe: connected — streaming temperature`, then
      live temps; drive >100 °C and <10 °C to confirm the parse.
- [ ] Map the probe → a grill's meat channel in `bbq_controller` (single channel;
      decide the assignment UX).
- [ ] Multi-probe: `MAX_CONNECTIONS`/state currently assume one thermometer.
- [ ] **Unify the BLE stack with `feature/BLEBoxIntegration`.** That branch is an
      OBSERVER (passive box adverts); this is a CENTRAL (probe connection). On
      merge there must be one NimBLE host doing both — passively decode box
      adverts *and* keep this probe connection alive.
- [ ] DRAM: central + connection uses more internal RAM than an observer — the
      tightest BLE case on this already-constrained build. Watch first boot.
