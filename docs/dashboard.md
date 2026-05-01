# Dashboard & HTTP API

The device runs an `esp_http_server` on **port 80** as soon as WiFi associates. The base URL is `http://<friendly-slug>.local/` (e.g. `http://altavoces-salon.local/`); the slug is derived from the friendly device name with diacritics folded and runs of non-alphanumerics squashed to `-`. The IP works equally well if mDNS is unavailable.

The web layer lives entirely in [`main/network/web_server.c`](../main/network/web_server.c). HTML pages are embedded in flash via `EMBED_FILES` so they survive any reflash and don't depend on SPIFFS.

## Pages

| URL | What it does |
|---|---|
| `/` | Single-page dashboard (`main/network/index.html`). Now-playing card with album art, status (network/IP/MAC/firmware/heap), 15-band EQ with 5 presets, device-name editor, restart button. ~12 KB. |
| `/logs` | Live log viewer (`main/network/logs.html`). WebSocket-streamed device logs with level coloring (I/W/E/D), substring filter, pause, autoscroll, exponential reconnect backoff. ~5 KB. |
| `/eq` | Redirect to `/` (the EQ controls are inline on the home page). |

## JSON API

| Method | URL | Description |
|---|---|---|
| GET | `/api/system/info` | Device IP, MAC, friendly name, network state, free heap, firmware version, `eq_supported` flag. |
| GET | `/api/now_playing` | RTSP-derived metadata: state (`idle`/`connected`/`playing`/`paused`), title, artist, album, genre, duration_secs, position_secs, has_artwork, artwork_etag. State is cached in `main/now_playing.c` and updated from the RTSP event bus. |
| GET | `/api/artwork.jpg` | The most recent album cover the iPhone pushed via RTSP `SET_PARAMETER`. Served from PSRAM with an FNV-1a content-derived `ETag` and respects `If-None-Match` for cheap polling. 404 when no artwork is cached yet. |
| GET | `/api/eq` | Current 15-band EQ gains as `gains_db: [..15 floats..]`. |
| POST | `/api/eq` | Body: `{"gains":[..15 numbers..]}`. Each gain clamped to ±15 dB. Emits an `eq_events` event; the audio task picks up new biquad coefficients atomically and the settings layer persists to NVS. |
| GET | `/api/wifi/scan` | Trigger a synchronous WiFi scan; returns SSID list with RSSI + channel. |
| POST | `/api/wifi/config` | Body: `{"ssid":"...","password":"..."}`. Persists to NVS and reboots. **Unauthenticated on the LAN.** |
| GET | `/api/device/name` | (via `system/info`'s `device_name` field) |
| POST | `/api/device/name` | Body: `{"name":"..."}`. Updates NVS; mDNS hostname slug refreshes on next boot. |
| POST | `/api/system/restart` | Sends an empty JSON OK then `esp_restart()` after 500 ms. |
| POST | `/api/ota/update` | Raw firmware binary in the request body. Receives into PSRAM, validates SHA-256, writes to the inactive OTA partition, swaps boot partition, restarts. After the new image stays healthy for 60 s of network uptime, the partition is marked valid and rollback is canceled. |
| GET / POST / DELETE | `/api/fs/{list,upload,delete}` | SPIFFS file ops, scoped to the `/spiffs/` prefix. |
| WebSocket | `/ws/logs` | Streams the in-RAM log ring buffer in real time. Up to 3 concurrent clients (`MAX_WS_CLIENTS` in [`main/network/log_stream.c`](../main/network/log_stream.c)). |

## Captive portal

When STA hasn't associated yet, the device advertises the AP `ESP32-AirPlay-Setup` and runs a DNS responder that points common captive-portal probes (Apple, Android, Windows) at `192.168.4.1`. Browsers auto-open the dashboard. Once STA gets an IP the AP is torn down and the captive DNS server stops.

## Auth

There is **no authentication** on any endpoint. The threat model is residential LAN — anyone on the same WiFi can hit the device. The OTA endpoint accepts unsigned firmware; this is documented in [`docs/review/security.md`](review/security.md) as a HIGH-severity risk you've accepted for personal/home use. If the device ever moves to a less-trusted network, the security review's findings need revisiting.

## Curl recipes

```bash
# Status snapshot
curl http://altavoces-salon.local/api/system/info | jq

# Push a Loudness EQ curve
curl -X POST -H "Content-Type: application/json" \
  -d '{"gains":[6,5,4,3,2,0,-1,-2,-1,0,1,2,3,4,5]}' \
  http://altavoces-salon.local/api/eq

# Reset EQ to flat
curl -X POST -H "Content-Type: application/json" \
  -d '{"gains":[0,0,0,0,0,0,0,0,0,0,0,0,0,0,0]}' \
  http://altavoces-salon.local/api/eq

# Save the album cover the iPhone is pushing
curl -o cover.jpg http://altavoces-salon.local/api/artwork.jpg

# Rename
curl -X POST -H "Content-Type: application/json" \
  -d '{"name":"Living Room Speaker"}' \
  http://altavoces-salon.local/api/device/name

# OTA over WiFi
curl -X POST --data-binary @build/airplay2-receiver.bin \
  http://altavoces-salon.local/api/ota/update

# Tail logs
python3 -c "
import asyncio, websockets
async def go():
    async with websockets.connect('ws://altavoces-salon.local/ws/logs') as ws:
        async for msg in ws: print(msg, end='')
asyncio.run(go())"
```

## Embedding new pages

The HTML files are listed in `EMBED_FILES` in [`main/CMakeLists.txt`](../main/CMakeLists.txt). The build wraps each into an inlined symbol pair (`_binary_<name>_html_start` / `_end`) which `web_server.c` references via `extern const char[] asm("...")`. To add a page: drop the HTML in `main/network/`, add it to `EMBED_FILES`, add the externs and a handler in `web_server.c`, register the URI.

The chime PCM and EQ now-playing JSON are wired the same way.
