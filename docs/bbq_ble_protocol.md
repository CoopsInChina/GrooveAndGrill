# BBQ BLE Thermometer — Protocol Reference

> Reverse-engineered from a physical device via nRF Connect GATT dumps and
> notification captures. Status: **payload format solid, prefix byte unresolved.**
> Treat sections marked ⚠️ as unverified assumptions, not facts.

## Purpose

Context for building a BLE client that reads live temperature from this device.
Everything here was derived from passive observation of one unit — no vendor
docs, no app decompile. The temperature parse is reliable; the leading prefix
byte is not yet understood, so do not build logic that depends on its meaning.

## Device identity

- **Advertised name:** `BBQ`
- **MAC:** `AB:BC:DD:02:99:A1` — last 3 bytes (`02:99:A1`) match the serial tail, so the address is derived from the unit ID.
- **Serial string** (from manufacturer data): `CXL-2404-0299A1`
  - `CXL` = ODM prefix
  - `2404` = build date, April 2024
  - `0299A1` = unit serial
- **Manufacturer company ID:** `0x55AA` — **not** a Bluetooth SIG–assigned ID; a placeholder magic number typical of an unregistered Chinese ODM. Do not look it up; treat the manufacturer payload as vendor-defined ASCII.

## GATT structure

Only one characteristic matters. No control/command characteristic exists, and
**no init write is required** — notifications stream as soon as you subscribe
(CCCD is effectively pre-enabled).

| Service | UUID | Notes |
|---|---|---|
| Generic Access | `0x1800` | standard |
| Generic Attribute | `0x1801` | standard |
| Device Information | `0x180A` | ⚠️ DIS strings (model/firmware/chipset) **not yet read** — worth grabbing |
| Custom (temperature) | `18424398-7cbc-11e9-8f9e-2a86e4085a59` | v1 time-based UUID, generated ~May 2019, random node — auto-generated, no vendor structure to exploit |

### Temperature characteristic

- **UUID:** `772ae377-b3d2-ff8e-1042-5481d1e03456`
  - ⚠️ This UUID is **malformed** (invalid version/variant nibbles) and shares no base structure with the service UUID. It was almost certainly hand-typed. Do not try to derive sibling UUIDs by pattern — there are none.
- **Properties:** `NOTIFY` only
- **Descriptor `0x2902`** (CCCD): notifications enabled
- **Descriptor `0x2901`** (User Description): `"Temperature sensor data"`
- **Notification interval:** ~1.56 s (occasional whole-multiple gaps observed, i.e. skipped slots, not jitter)

## Payload format

Raw value is **ASCII text**, not binary:

```
[1 prefix byte][decimal temperature string]
```

Observed samples (hex → decoded):

| Hex | ASCII | Temp (°C) |
|---|---|---|
| `40 33 31 2E 34` | `@31.4` | 31.4 |
| `3B 32 39 2E 36` | `;29.6` | 29.6 |
| `45 32 39 2E 30` | `E29.0` | 29.0 |

- Temperature = ASCII decimal, always seen with one decimal place.
- ⚠️ **Field width above 100 °C / below 10 °C / negative is untested.** Every capture so far has been two-digit. Do not assume fixed width. Verify before trusting in a real cook or for fridge/freezer use.

### The prefix byte — UNRESOLVED ⚠️

This is the open problem. Do not encode any behaviour that depends on decoding it.

**What's known:**
- Constant within a single session, but varies across sessions.
- Observed values: `@ B c d e E` = `0x40 0x42 0x63 0x64 0x65 0x45`, plus `;` = `0x3B`.

**Ruled out** (by observation):
- Channel ID — device has only ONE temperature channel (no grill temp), and the byte never alternates within a session.
- Checksum — stays constant while the temperature payload changes.
- Per-notification counter — same reason.
- Temperature-linked — the same temperature appeared under different prefixes across sessions.

**Still possible:**
- Probe identity / curve ID (follows the probe)
- Status / state code
- Session state (assigned fresh each connection)

**Caveat on the parser mask:** an earlier working theory was `0x40 | field`,
which suggested `data[0] & 0x3F`. The `;` = `0x3B` sample breaks that — it sits
*below* `0x40`, so masking with `0x3F` may strip a meaningful bit. Keep the raw
prefix byte around; don't collapse it until it's understood.

## Working parser

Temperature extraction is reliable. Prefix is preserved raw for later analysis
rather than interpreted.

```python
from dataclasses import dataclass

@dataclass
class Reading:
    prefix: int        # raw prefix byte — meaning UNRESOLVED, keep for analysis
    temp_c: float

def parse(data: bytes) -> Reading:
    """Parse a notification from characteristic 772ae377-...-d1e03456.

    Payload is ASCII: one prefix byte + decimal temperature string.
    Only the temperature is trusted; the prefix is stored raw.
    """
    return Reading(prefix=data[0], temp_c=float(data[1:].decode("ascii")))
```

Minimal bleak subscriber (matches the established stack for this project):

```python
import asyncio
from bleak import BleakClient, BleakScanner

CHAR_UUID = "772ae377-b3d2-ff8e-1042-5481d1e03456"

def _on_notify(_handle, data: bytes):
    r = parse(data)
    print(f"prefix=0x{r.prefix:02X} temp={r.temp_c:.1f}C")

async def main():
    dev = await BleakScanner.find_device_by_name("BBQ")
    async with BleakClient(dev) as client:
        await client.start_notify(CHAR_UUID, _on_notify)
        await asyncio.sleep(60)          # stream for a minute
        await client.stop_notify(CHAR_UUID)

asyncio.run(main())
```

## Outstanding tests (to close the unknowns)

Run in this order; the prefix field should fall out in ~2 minutes:

1. **Reconnect, same probe + same port.** Prefix changes → it's session state (safe to ignore). Prefix stable → it's physical, keep investigating.
2. **Same probe, move to another port** (if the unit has more than one). Prefix changes → low bits are the port.
3. **Swap to a different probe, same port.** Prefix changes → it's probe identity.
4. **Read the DIS (`0x180A`) strings** — model / firmware / chipset. Cheap while connected; may identify a rebadged ODM board someone else has already documented.
5. **Drive temperature past 100 °C and below 10 °C / negative** — confirm field width and sign handling before trusting the parser in production.

## Integration notes

- Drop this file at repo root as part of `CLAUDE.md`, or keep it separate and reference it (e.g. `@bbq_ble_protocol.md`) from your main context file.
- The parser and GATT UUIDs are safe to build against now.
- Anything marked ⚠️ is an assumption — surface it rather than silently coding around it, and update this doc once the outstanding tests resolve it.
