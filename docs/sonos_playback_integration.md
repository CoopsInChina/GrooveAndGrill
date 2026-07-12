# Sonos Playback Integration — removing the `node-sonos-http-api` dependency

Goal: play Sonos **favourites** (and keep transport control working) **without**
the external `node-sonos-http-api` server, by talking UPnP/SOAP directly to the
speaker (port 1400) from the ESP32.

This document is the analysis of how the reference stack works and a concrete
plan to fold the needed pieces into `sonos_controller.c`.

---

## 1. What we depend on the external server for today

`sonos_controller.c` already talks **direct UPnP SOAP** to the speaker for
play / pause / next / previous / volume / transport state, and already does a
`Browse FV:2` to fetch favourite **artwork**. The external
`node-sonos-http-api` (`g_api_server`, port 5005) is still used for only two
things:

| Function | Current call | File ref |
|---|---|---|
| **List** built-in favourites (names) | `GET http://<api>/favorites` | `fetch_favourites()` |
| **Play** a built-in favourite | `GET http://<api>/<room>/favourite/<name>` | `do_play_favourite()` |
| **Play** a device/custom favourite | `GET http://<api>/<room>/<cmd>` | `do_play_favourite()` |

Everything else is already server-free. So the integration is narrow: replace
**list** and **play-favourite** with direct SOAP.

---

## 2. How the reference stack actually does it

`node-sonos-http-api` is a thin HTTP wrapper; the real work is in its
`sonos-discovery` dependency (v1.8.0). Verified from source:

- `favorites` action → `player.system.getFavorites()` → `getAnyPlayer().browseAll('FV:2')`
- `favorite` action → `player.coordinator.replaceWithFavorite(name).then(play)`

### 2a. SOAP transport (all actions share this shape)

- **Endpoints** (`http://<speaker-ip>:1400` + control path):
  - AVTransport → `/MediaRenderer/AVTransport/Control`
  - RenderingControl → `/MediaRenderer/RenderingControl/Control`
  - ContentDirectory → `/MediaServer/ContentDirectory/Control`
- **POST headers:** `CONTENT-TYPE: text/xml; charset="utf-8"`,
  `SOAPACTION: "urn:schemas-upnp-org:service:<Service>:1#<Action>"`
- **Body envelope:**
  ```xml
  <s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/"
      s:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/">
    <s:Body> <!-- action element here --> </s:Body>
  </s:Envelope>
  ```

`sonos_controller.c` already builds exactly this (see its `soap_invoke` helper),
so all new actions reuse that path.

### 2b. Listing favourites — `Browse FV:2`

Action `ContentDirectory:1#Browse`, body:
```xml
<u:Browse xmlns:u="urn:schemas-upnp-org:service:ContentDirectory:1">
  <ObjectID>FV:2</ObjectID>
  <BrowseFlag>BrowseDirectChildren</BrowseFlag>
  <Filter></Filter>
  <StartingIndex>0</StartingIndex>
  <RequestedCount>0</RequestedCount>
  <SortCriteria></SortCriteria>
</u:Browse>
```
The response `<Result>` is **escaped DIDL-Lite XML**. Each favourite is an
`<item>` with the fields we need:

| DIDL field | Meaning | Use |
|---|---|---|
| `<dc:title>` | display name | list label |
| `<res>` | the favourite **uri** | what to play |
| `<r:resMD>` | the favourite **metadata** (a nested DIDL doc) | passed on playback |
| `<upnp:albumArtURI>` | art path | already parsed today for art |

> We already parse this exact response for art — we just additionally capture
> `<res>` (uri) and `<r:resMD>` (metadata) per item.

`browseAll` just loops `Browse` with `StartingIndex += numberReturned` until
`startIndex + numberReturned >= totalMatches`. For ≤20 favourites a single
`RequestedCount>0` (server default) is typically enough.

### 2c. Playing a favourite — `replaceWithFavorite`

Two paths, chosen by the favourite's uri prefix:

**Radio / stream** (uri starts with any of):
`x-sonosapi-stream:`, `x-sonosapi-radio:`, `x-sonosapi-hls:`,
`x-sonosprog-http:`, `pndrradio:`, `x-rincon-mp3radio:`
→ single call: `SetAVTransportURI(uri, resMD)` then `Play`.

**Track / playlist / album** (everything else):
1. `RemoveAllTracksFromQueue` (clear the queue)
2. `AddURIToQueue(uri, resMD, DesiredFirstTrackNumberEnqueued=0, EnqueueAsNext=0)`
3. `SetAVTransportURI("x-rincon-queue:<UUID>#0", "")` — point transport at the queue
4. `Play`

`<UUID>` is the speaker's RINCON id (e.g. `RINCON_XXXXXXXX01400`), already known
from discovery / device description.

### 2d. Exact SOAP bodies to add

```xml
<!-- AVTransport:1#SetAVTransportURI -->
<u:SetAVTransportURI xmlns:u="urn:schemas-upnp-org:service:AVTransport:1">
  <InstanceID>0</InstanceID>
  <CurrentURI>{uri}</CurrentURI>
  <CurrentURIMetaData>{metadata}</CurrentURIMetaData>
</u:SetAVTransportURI>

<!-- AVTransport:1#RemoveAllTracksFromQueue -->
<u:RemoveAllTracksFromQueue xmlns:u="urn:schemas-upnp-org:service:AVTransport:1">
  <InstanceID>0</InstanceID>
</u:RemoveAllTracksFromQueue>

<!-- AVTransport:1#AddURIToQueue -->
<u:AddURIToQueue xmlns:u="urn:schemas-upnp-org:service:AVTransport:1">
  <InstanceID>0</InstanceID>
  <EnqueuedURI>{uri}</EnqueuedURI>
  <EnqueuedURIMetaData>{metadata}</EnqueuedURIMetaData>
  <DesiredFirstTrackNumberEnqueued>0</DesiredFirstTrackNumberEnqueued>
  <EnqueueAsNext>0</EnqueueAsNext>
</u:AddURIToQueue>

<!-- AVTransport:1#Play -->
<u:Play xmlns:u="urn:schemas-upnp-org:service:AVTransport:1">
  <InstanceID>0</InstanceID><Speed>1</Speed>
</u:Play>
```

**Critical:** `{uri}` and `{metadata}` must be **XML-entity-encoded** before
substitution (the metadata is itself an escaped XML document, so it's
double-encoded in the body). The reference does `xmlEntities.encode()` on both.

---

## 3. Gaps / things to get right

- **XML entity encoding** of uri + metadata (`&`, `<`, `>`, `"`, `'`). This is
  the most error-prone part; the `resMD` blob contains lots of `&lt;`/`&amp;`.
- **DIDL parsing** must unescape `<Result>` then extract `res` / `r:resMD` /
  `dc:title` per `<item>`. We already stream-parse this for art — extend it.
- **Buffer sizes:** a favourite's `resMD` can be 1–3 KB; the full `FV:2` result
  is 15–30 KB for 20 favourites (already accounted for — PSRAM browse buffer).
- **Speaker UUID:** needed for the `x-rincon-queue:<UUID>#0` transport uri;
  confirm it's captured during discovery (device_description.xml `<UDN>`).
- **Coordinator vs member:** reference plays on `player.coordinator`. For a
  single speaker (our case) the speaker is its own coordinator, so direct is
  fine; grouped-zone support can come later.
- **Custom/device favourites** (NVS-stored Spotify links added via `/setup`):
  these currently play via a stored API command string. They'll need their own
  direct-play path — build the `uri`/`metadata` for a Spotify track/playlist
  (see `sonos-discovery/lib/services/spotify.js` and the http-api
  `music_services/spotifyDef.js` for the metadata template). This is the
  larger sub-task and can follow the built-in-favourites work.

---

## 4. Proposed implementation steps

1. **Capture uri + resMD** in the existing `FV:2` Browse parser (alongside art),
   into the favourites model.
2. Add an **XML-entity-encode** helper.
3. Add `sonos_play_favourite_direct(idx)` implementing §2c using the existing
   `soap_invoke` helper (radio path + queue path).
4. Replace `fetch_favourites()`'s name source: derive names from the same
   `FV:2` Browse instead of `GET /favorites` — one Browse now yields
   name + uri + metadata + art together.
5. Switch `do_play_favourite()` for built-in favourites to the direct path;
   keep the API path behind a fallback flag until verified on hardware.
6. **Later:** direct-play for NVS custom (Spotify) favourites; drop
   `g_api_server` entirely once both paths are proven.

Reference sources (local clones, not committed):
`~/node-sonos-http-api` and `~/node-sonos-discovery` (v1.8.0) —
key files `lib/helpers/soap.js`, `lib/models/Player.js`,
`lib/prototypes/Player/replaceWithFavorite.js`,
`lib/prototypes/SonosSystem/getFavorites.js`.
