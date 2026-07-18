# BBQ Box BLE receiver integration

Consumes the [ESP-BLE-BBQ-Box](https://github.com/CoopsInChina/ESP-BLE-BBQ-Box)
satellite: a passive BLE scan reads its connectionless advertisements and feeds
live thermocouple temperatures into `bbq_controller`, replacing the mock.

## Pieces

- **`bbq_ble.c/.h`** — NimBLE **observer** (passive scan). Parses the box's
  manufacturer payload (company `0xFFFF`, version `0x01`, fault byte, 4× uint16
  LE in 0.1 °C) per the box's `docs/ble_protocol.md`. Keeps the latest 4 temps +
  per-channel validity + last-heard timestamp. `bbq_ble_channel(ch, &t)` returns
  a temperature only when the box is fresh (heard < 5 s ago) and that channel
  isn't faulted. Logs first contact + a 5 s heartbeat for verification.
- **`bbq_controller.c`** — a 1 Hz `esp_timer` maps the 4 channels onto grills:
  **grill 0 = TC0 (grill) + TC1 (meat), grill 1 = TC2 + TC3**. Sets each grill's
  `grill_temp_c`/`meat_temp_c` and drives `probe_state`
  (connected while a channel reads; → disconnected when a live channel drops).
  Only runs while the box is present, so the mock/UI state is untouched on the
  bench when no box is powered.
- **`sdkconfig.defaults`** — enables NimBLE observer role; Classic-BT controller
  memory is released at init.

## Status / to-do

- [ ] **First build — watch DRAM.** BLE controller + host need internal DRAM and
      this build is already tight (see `LV_MEM_SIZE_KILOBYTES` history). If tasks
      fail to spawn / BLE init fails, reclaim DRAM (drop LVGL pool, trim NimBLE
      buffers) before anything else.
- [ ] Verify reception: serial should log `bbq_ble: box found — TC0=… …`.
- [ ] Confirm live temps drive the BBQ gauges end-to-end.
- [ ] TC→grill mapping is a fixed v1 default — make it configurable, and decide
      auto-creating grill 1 when TC2/TC3 report.
- [ ] Retire the `bbq_mock_*` hooks once the real path is proven.
- [ ] Multi-box support (match by MAC) if more than one satellite is ever used.
