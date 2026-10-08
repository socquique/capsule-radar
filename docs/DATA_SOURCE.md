# Data source — ADS-B feed

## Primary: airplanes.live (contributor-only)
Independent, community-owned ADS-B/MLAT aggregator. **Educational / non-commercial use only** — fits this project. Be a good citizen: poll every 1–2 s max, send a descriptive `User-Agent`.

> **Access is now contributor-only (since Sept 2026), granted by IP.** If you run one of
> their feeders (an ADS-B receiver contributing data — see
> <https://airplanes.live/get-started/>), every device on the same network gets the API
> automatically, this radar included: nothing to configure, no keys exist. Everyone else
> gets `403`; the firmware then parks this provider with an **escalating backoff (15 min
> doubling up to 6 h)** and runs on the two below — start feeding and the provider comes
> back within hours, or immediately after a reboot. Running a feeder is also the way to
> support them; their hosting costs are real.

### Endpoint (position + radius)
```
GET https://api.airplanes.live/v2/point/{lat}/{lon}/{radius_nm}
```
- `lat`, `lon`: decimal degrees (our home coords).
- `radius_nm`: nautical miles (max 250). Convert from our range: `nm = km * 0.539957`.

### Response (readsb format)
JSON object; the aircraft list is under key **`ac`** (older/raw readsb files use `aircraft` — handle both). Each entry includes (keys omitted when unavailable):

| Field          | Meaning                                  | Use |
|----------------|------------------------------------------|-----|
| `hex`          | 24-bit ICAO id (may start with `~`)      | stable key / de-dupe |
| `flight`       | callsign (8 chars)                       | label |
| `lat`,`lon`    | position, decimal degrees                | project to screen |
| `alt_baro`     | barometric altitude (ft) or `"ground"`   | altitude color |
| `track`        | ground track, ° from true N             | glyph rotation |
| `true_heading` | heading, ° (fallback when no `track`)   | glyph rotation |
| `gs`           | ground speed (kt)                        | detail card |
| `baro_rate`    | vertical rate (fpm, ±)                   | V/S arrow |
| `squawk`       | transponder code                         | emergency detect (7500/7600/7700) |
| `seen_pos`     | seconds since last position fix          | stale/expiry + trail |
| `t` / `type`   | aircraft type (e.g. B738) when present   | detail card |
| `dbFlags`      | bitfield (military, etc.)                | "interesting" alerts |

### Second: adsb.fi (opendata)
Same readsb `ac` payload, **different URL shape**:
`GET https://opendata.adsb.fi/api/v3/lat/{lat}/lon/{lon}/dist/{radius_nm}` (max 250 NM).

Rate limit is documented and fixed at **1 request per second** for the public endpoints,
which makes it the most predictable of the three.

> **Attribution is required.** adsb.fi ask that you *"cite adsb.fi and include a link to our
> home page"*, and the service is for **personal, non-commercial use only**, provided as-is
> without warranty. Home page: <https://adsb.fi/>. Keep that credit in the README and in any
> listing (MakerWorld etc.) that ships this firmware.

### Fallback: adsb.lol
Same readsb format. `GET https://api.adsb.lol/v2/point/{lat}/{lon}/{radius_nm}`. Wire it as an automatic failover if airplanes.live errors/times out.

Its rate limits are **dynamic** ("based on the environment load"), so there is no fixed rate
to code against — the firmware adapts its spacing on each 429 rather than assuming a number.
adsb.lol also rejects a generic `User-Agent` outright (403 *"User-Agent too generic; include
valid contact info"*), which is why the UA must carry a project link.

## Provider pacing (how the firmware behaves)
Providers are tried in order and paced **individually**:
- **403** — a policy refusal (needs approval, or a rejected User-Agent). Parked for 15 min,
  doubling up to 6 h; retrying it every poll only burns requests and pushes the others over
  their limits.
- **429** — too fast. An adaptive minimum spacing doubles on each 429 and eases back after a
  run of successes. An explicit `Retry-After` wins.
- **200 with no aircraft array** — the response parsed but is unusable (a provider changed its
  payload shape, or the chunked-encoding trap below). Same backoff as a 429: without it such a
  provider is re-asked every poll forever, which is exactly the loop that hid the adsb.fi
  chunking bug.
- **No answer at all** (connect, TLS or read timeout) — the first one is free; from the second
  in a row the provider waits a doubling gap (4 s up to 60 s), so one dead host cannot
  monopolise the poll budget. The gap clears on the provider's first usable answer, so a short
  internet outage does not leave the feed slow afterwards. While **every** provider is silent
  (parked, or failing without an answer) the gap is capped at 10 s
  (`ADSB_SILENCE_OUTAGE_MAX_MS`), so after a network-wide outage the next poll is at most 10 s
  late rather than up to a minute. As soon as any provider answers, the others wait their full
  gap again; the 429 spacing is never capped.

The policy lives in `src/adsb_pacing.h`, separate from the HTTP code so it can be driven on
the host — `tests/adsb_pacing_test.cpp` exercises parking, backoff, easing, `Retry-After` and
`millis()` rollover without hardware.

The **self-heal watchdog** (`FeedWatchdog` in `src/feed_watchdog.h`, run by `adsb_task`) targets
one failure: the internal heap fragmenting until TLS can no longer allocate. It restarts the device
when the feed has been stuck for 180 s (`ADSB_STUCK_MS`) with WiFi up **and** the largest
free internal heap block is below `ADSB_STUCK_MIN_LARGEST_BLOCK` (28 KB). Any completed HTTP
exchange (`AdsbClient::lastResponseMs()`, so a 403 counts) and WiFi being down refresh it. A poll
the pacer skipped does **not**: fast TLS failures open the silence gaps above, the poll skips
inside them, and counting those skips kept the restart from ever coming. A feed stuck for any
other reason — every provider refusing us, or the internet down behind a working WiFi — keeps the
radar running with the amber HUD warning instead of rebooting it every three minutes, but not
forever: a backstop restarts the device regardless of the heap once WiFi has been up and no
provider has answered for more than 30 minutes (`ADSB_STUCK_HARD_MS`). Any answer in that time
resets the clock, so an outage that ends sooner never triggers it. The backstop's clock also
stands still while **every** provider is parked by a refusal or `Retry-After`
(`AdsbClient::allParked()`): that silence is our own, and the second 403 park (30 min) would
otherwise end just after the backstop and reboot a refused device before it could knock again.
`tests/feed_watchdog_test.cpp` drives the watchdog and the real pacer through these cases.

Every HTTPS client sets `setHandshakeTimeout(TLS_HANDSHAKE_S)` (10 s). The core default is 120 s,
so a server that accepts TCP but never finishes the handshake would hold the network task for two
minutes, and two such hangs in a row outlast the 180 s watchdog with nothing wrong on the device.
The JSON documents and the mbedTLS buffers themselves live in PSRAM (the PSRAM `JsonDocument`
allocator in `adsb_client.cpp` and `mbedtls_platform_set_calloc_free()` in `main.cpp`), so TLS
traffic cannot fragment the internal heap the handshake needs.

Set the `User-Agent` with `HTTPClient::setUserAgent()`. **`addHeader("User-Agent", ...)` is
silently ignored** — Arduino keeps that header on an internal "handled by code" list, so the
request goes out as the default `ESP32HTTPClient` and providers refuse it.

Request with `HTTPClient::useHTTP10(true)`. We stream-parse straight off `http.getStream()`,
and that is the **raw socket** — Arduino only de-chunks inside `writeToStream()`/`getString()`.
adsb.fi replies `Transfer-Encoding: chunked` (adsb.lol sends `Content-Length`), so without
this ArduinoJson reads the hex chunk-size line first, parses `4000` as a valid JSON number,
and returns success with no `ac` key: a 200 that silently yields no aircraft. HTTP/1.0 has no
chunked encoding, so the body is Content-Length- or close-delimited and streams correctly.

### Not used: OpenSky
Now requires OAuth2 client-credentials and has tighter anonymous limits — awkward for an always-on embedded device. Keep as a documented alternative only.

## On-device math (implemented in src/geo.h)
For each aircraft, given home `(lat0, lon0)` and range `R_km` (outer ring):
1. **Distance** via haversine (km).
2. **Bearing** from home to aircraft (° from N, clockwise).
3. **Project** to screen: `r_px = (dist_km / R_km) * R_px_outer`; with north-up,
   `x = cx + r_px * sin(bearing)`, `y = cy - r_px * cos(bearing)`.
4. Drop aircraft beyond `R_km` (or clamp to the rim with a "beyond range" marker).
