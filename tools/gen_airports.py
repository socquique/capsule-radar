#!/usr/bin/env python3
"""Generate src/airports_data.h from the OurAirports airports.csv.

Keeps large airports, medium airports that have an IATA code, and small airports
that either have a recognizable IATA/local code plus runway endpoint geometry, or
have asphalt runway endpoint geometry. That keeps useful runway-equipped fields
without embedding every tiny strip. Emits int16 lat/lon (degrees * 100), a
4-character GPS/ICAO label when available, and a 'large' flag.

Usage:
    python3 tools/gen_airports.py /tmp/airports.csv src/airports_data.h /tmp/runways.csv

Source data: OurAirports (public domain). https://ourairports.com/data/
"""
import csv
import json
import math
import sys
import unicodedata

SCALE = 100.0
M_PER_DEG_LAT = 111320.0


def clamp_i16(v):
    return max(-32768, min(32767, int(round(v))))


def cstr(s, limit):
    s = " ".join((s or "").replace("\n", " ").split())
    s = unicodedata.normalize("NFKD", s).encode("ascii", "ignore").decode("ascii")
    if len(s) > limit - 1:
        s = s[:limit - 2].rstrip() + "~"
    return json.dumps(s)


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else "/tmp/airports.csv"
    dst = sys.argv[2] if len(sys.argv) > 2 else "src/airports_data.h"
    runways_src = sys.argv[3] if len(sys.argv) > 3 else "/tmp/runways.csv"
    freqs_src = sys.argv[4] if len(sys.argv) > 4 else "/tmp/airport-frequencies.csv"

    small_with_runways = set()
    small_with_asphalt = set()
    try:
        with open(runways_src, newline="", encoding="utf-8") as f:
            for r in csv.DictReader(f):
                if (r.get("closed") or "0").strip() == "1":
                    continue
                try:
                    float(r["le_latitude_deg"])
                    float(r["le_longitude_deg"])
                    float(r["he_latitude_deg"])
                    float(r["he_longitude_deg"])
                except (ValueError, KeyError):
                    continue
                ident = (r.get("airport_ident") or "").strip().upper()
                if ident:
                    small_with_runways.add(ident)
                    if (r.get("surface") or "").strip().upper() == "ASP":
                        small_with_asphalt.add(ident)
    except FileNotFoundError:
        pass

    rows = []
    airport_by_ident = {}
    with open(src, newline="", encoding="utf-8") as f:
        for r in csv.DictReader(f):
            t = r["type"]
            iata = (r.get("iata_code") or "").strip().upper()[:3]
            gps = (r.get("gps_code") or "").strip().upper()[:4]
            ident = (r.get("ident") or "").strip().upper()[:4]
            local = (r.get("local_code") or "").strip().upper()[:4]
            label = gps or ident or iata
            name = r.get("name") or label
            if t == "large_airport":
                large = 1
                airport_class = 2
            elif t == "medium_airport" and iata:
                large = 0
                airport_class = 1
            elif t == "small_airport" and ident in small_with_runways and ((iata or local) or ident in small_with_asphalt):
                large = 0
                airport_class = 0
            else:
                continue
            try:
                lat = float(r["latitude_deg"])
                lon = float(r["longitude_deg"])
            except (ValueError, KeyError):
                continue
            lat_s = max(-9000, min(9000, round(lat * SCALE)))
            lon_s = max(-18000, min(18000, round(lon * SCALE)))
            airport_by_ident[ident] = len(rows)
            rows.append((lat_s, lon_s, label, large, airport_class, lat, lon, ident, name))

    runways = []
    runway_names = [[] for _ in rows]
    runway_surfaces = [[] for _ in rows]
    try:
        with open(runways_src, newline="", encoding="utf-8") as f:
            for r in csv.DictReader(f):
                if (r.get("closed") or "0").strip() == "1":
                    continue
                ap_idx = airport_by_ident.get((r.get("airport_ident") or "").strip().upper())
                if ap_idx is None:
                    continue
                try:
                    le_lat = float(r["le_latitude_deg"])
                    le_lon = float(r["le_longitude_deg"])
                    he_lat = float(r["he_latitude_deg"])
                    he_lon = float(r["he_longitude_deg"])
                except (ValueError, KeyError):
                    continue

                _, _, _, _, _, ap_lat, ap_lon, _, _ = rows[ap_idx]
                m_per_deg_lon = M_PER_DEG_LAT * math.cos(math.radians(ap_lat))
                le_e = clamp_i16((le_lon - ap_lon) * m_per_deg_lon)
                le_n = clamp_i16((le_lat - ap_lat) * M_PER_DEG_LAT)
                he_e = clamp_i16((he_lon - ap_lon) * m_per_deg_lon)
                he_n = clamp_i16((he_lat - ap_lat) * M_PER_DEG_LAT)
                runways.append((ap_idx, le_e, le_n, he_e, he_n))
                le = (r.get("le_ident") or "").strip()
                he = (r.get("he_ident") or "").strip()
                if le and he:
                    runway_names[ap_idx].append("%s/%s" % (le, he))
                surface = (r.get("surface") or "").strip().upper()
                if surface:
                    runway_surfaces[ap_idx].append(surface)
    except FileNotFoundError:
        pass
    runways.sort(key=lambda r: r[0])

    runway_first = [-1] * len(rows)
    runway_count = [0] * len(rows)
    for i, r in enumerate(runways):
        ap_idx = r[0]
        if runway_first[ap_idx] < 0:
            runway_first[ap_idx] = i
        runway_count[ap_idx] += 1

    freq_priority = {"TWR": 0, "GND": 1, "CTAF": 2, "UNIC": 3, "ATIS": 4, "A/D": 5, "APP": 6, "DEP": 7, "CLD": 8}
    freqs = [[] for _ in rows]
    try:
        with open(freqs_src, newline="", encoding="utf-8") as f:
            for r in csv.DictReader(f):
                ap_idx = airport_by_ident.get((r.get("airport_ident") or "").strip().upper())
                if ap_idx is None:
                    continue
                typ = (r.get("type") or "").strip().upper()
                if typ not in freq_priority:
                    continue
                mhz = (r.get("frequency_mhz") or "").strip()
                if not mhz:
                    continue
                freqs[ap_idx].append((freq_priority[typ], "%s %s" % (typ, mhz)))
    except FileNotFoundError:
        pass

    runway_text = []
    surface_text = []
    freq_text = []
    for i in range(len(rows)):
        runway_text.append(", ".join(runway_names[i][:10]) if runway_names[i] else "-")
        unique_surfaces = []
        seen_surfaces = set()
        for surface in runway_surfaces[i]:
            if surface in seen_surfaces:
                continue
            seen_surfaces.add(surface)
            unique_surfaces.append(surface)
            if len(unique_surfaces) >= 5:
                break
        surface_text.append(", ".join(unique_surfaces) if unique_surfaces else "-")
        unique = []
        seen = set()
        for _, txt in sorted(freqs[i]):
            if txt in seen:
                continue
            seen.add(txt)
            unique.append(txt)
            if len(unique) >= 5:
                break
        freq_text.append(" | ".join(unique) if unique else "-")

    # large first isn't needed; keep file order. Stats:
    n_large = sum(1 for r in rows if r[4] == 2)
    n_medium = sum(1 for r in rows if r[4] == 1)
    with open(dst, "w") as f:
        f.write("// Auto-generated by tools/gen_airports.py — DO NOT EDIT.\n")
        f.write("// Source: OurAirports (public domain).\n")
        f.write("// %d airports (%d large, %d medium, %d small). lat,lon are int16 deg*%d.\n"
                % (len(rows), n_large, n_medium, len(rows) - n_large - n_medium, int(SCALE)))
        f.write("#pragma once\n#include <stdint.h>\n\n")
        f.write("#define AIRPORT_SCALE %d\n" % int(SCALE))
        f.write("#define AIRPORT_NUM %d\n\n" % len(rows))

        f.write("static const int16_t AIRPORT_LAT[AIRPORT_NUM] = {\n")
        for i in range(0, len(rows), 18):
            f.write("  " + ",".join(str(r[0]) for r in rows[i:i + 18]) + ",\n")
        f.write("};\n\n")

        f.write("static const int16_t AIRPORT_LON[AIRPORT_NUM] = {\n")
        for i in range(0, len(rows), 18):
            f.write("  " + ",".join(str(r[1]) for r in rows[i:i + 18]) + ",\n")
        f.write("};\n\n")

        f.write("// 1 = large airport, 0 = medium or small airport.\n")
        f.write("static const uint8_t AIRPORT_LARGE[AIRPORT_NUM] = {\n")
        for i in range(0, len(rows), 32):
            f.write("  " + ",".join(str(r[3]) for r in rows[i:i + 32]) + ",\n")
        f.write("};\n\n")

        f.write("// 2 = large airport, 1 = medium airport, 0 = small airport.\n")
        f.write("static const uint8_t AIRPORT_CLASS[AIRPORT_NUM] = {\n")
        for i in range(0, len(rows), 32):
            f.write("  " + ",".join(str(r[4]) for r in rows[i:i + 32]) + ",\n")
        f.write("};\n\n")

        f.write("// Preferred airport label: GPS/ICAO code when available, otherwise ident/IATA.\n")
        f.write("static const char AIRPORT_LABEL[AIRPORT_NUM][5] = {\n")
        for i in range(0, len(rows), 12):
            f.write("  " + ",".join('"%s"' % r[2] for r in rows[i:i + 12]) + ",\n")
        f.write("};\n\n")

        f.write("static const char AIRPORT_NAME[AIRPORT_NUM][48] = {\n")
        for i in range(0, len(rows), 4):
            f.write("  " + ",".join(cstr(r[8], 48) for r in rows[i:i + 4]) + ",\n")
        f.write("};\n\n")

        f.write("static const char AIRPORT_RUNWAYS_TXT[AIRPORT_NUM][96] = {\n")
        for i in range(0, len(rows), 4):
            f.write("  " + ",".join(cstr(v, 96) for v in runway_text[i:i + 4]) + ",\n")
        f.write("};\n\n")

        f.write("static const char AIRPORT_SURFACES_TXT[AIRPORT_NUM][32] = {\n")
        for i in range(0, len(rows), 4):
            f.write("  " + ",".join(cstr(v, 32) for v in surface_text[i:i + 4]) + ",\n")
        f.write("};\n\n")

        f.write("static const char AIRPORT_FREQS_TXT[AIRPORT_NUM][72] = {\n")
        for i in range(0, len(rows), 3):
            f.write("  " + ",".join(cstr(v, 72) for v in freq_text[i:i + 3]) + ",\n")
        f.write("};\n\n")

        f.write("// Index into RUNWAY_* arrays, or -1 when no endpoint geometry is available.\n")
        f.write("static const int16_t AIRPORT_RUNWAY_FIRST[AIRPORT_NUM] = {\n")
        for i in range(0, len(runway_first), 18):
            f.write("  " + ",".join(str(v) for v in runway_first[i:i + 18]) + ",\n")
        f.write("};\n\n")

        f.write("static const uint8_t AIRPORT_RUNWAY_COUNT[AIRPORT_NUM] = {\n")
        for i in range(0, len(runway_count), 32):
            f.write("  " + ",".join(str(v) for v in runway_count[i:i + 32]) + ",\n")
        f.write("};\n\n")

        f.write("#define RUNWAY_NUM %d\n\n" % len(runways))
        f.write("// Runway endpoints in meters east/north from the airport reference point.\n")
        f.write("static const int16_t RUNWAY_LE_E_M[RUNWAY_NUM] = {\n")
        for i in range(0, len(runways), 18):
            f.write("  " + ",".join(str(r[1]) for r in runways[i:i + 18]) + ",\n")
        f.write("};\n\n")
        f.write("static const int16_t RUNWAY_LE_N_M[RUNWAY_NUM] = {\n")
        for i in range(0, len(runways), 18):
            f.write("  " + ",".join(str(r[2]) for r in runways[i:i + 18]) + ",\n")
        f.write("};\n\n")
        f.write("static const int16_t RUNWAY_HE_E_M[RUNWAY_NUM] = {\n")
        for i in range(0, len(runways), 18):
            f.write("  " + ",".join(str(r[3]) for r in runways[i:i + 18]) + ",\n")
        f.write("};\n\n")
        f.write("static const int16_t RUNWAY_HE_N_M[RUNWAY_NUM] = {\n")
        for i in range(0, len(runways), 18):
            f.write("  " + ",".join(str(r[4]) for r in runways[i:i + 18]) + ",\n")
        f.write("};\n")

    bytes_total = len(rows) * (2 + 2 + 1 + 1 + 5 + 48 + 96 + 32 + 72 + 2 + 1) + len(runways) * (2 + 2 + 2 + 2)
    print("airports: %d (%d large, %d medium, %d small)" % (len(rows), n_large, n_medium, len(rows) - n_large - n_medium))
    print("runways:  %d" % len(runways))
    print("flash:    ~%.1f KB" % (bytes_total / 1024.0))
    print("written:  %s" % dst)


if __name__ == "__main__":
    main()
