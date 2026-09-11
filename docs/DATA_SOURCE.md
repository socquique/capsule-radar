# Data source — ADS-B feed

## Primary: airplanes.live (free, no key)
Independent ADS-B/MLAT aggregator. **Educational / non-commercial use only** — fits this project. Be a good citizen: poll every 1–2 s max, send a descriptive `User-Agent`.

> **Access now requires prior approval.** Unapproved clients get `403` with
> *"Please contact us at contact@airplanes.live. Your email MUST include any links, a
> description of the project, and any information you deem appropriate."* Until that is
> granted the firmware parks this provider and runs on the two below.

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
- **403** — a policy refusal (needs approval, or a rejected User-Agent). Parked for 15 min;
  retrying it every poll only burns requests and pushes the others over their limits.
- **429** — too fast. An adaptive minimum spacing doubles on each 429 and eases back after a
  run of successes. An explicit `Retry-After` wins.

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
