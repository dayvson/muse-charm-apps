#!/usr/bin/env python3
"""Generate firmware/muse_zones.c: city codes -> IANA name -> POSIX TZ rule.

The ESP32 has no tz database, so each zone carries the POSIX TZ string from
the footer of its TZif file (the last line of /usr/share/zoneinfo/<zone>).
Muse picks a zone by code ("TLV"), city name ("Tel Aviv") or IANA name
("Asia/Jerusalem"); anything else it can pass as a POSIX string itself.

Usage: gen_zones.py OUT.c
"""
import sys
from pathlib import Path

ZONEINFO = Path("/usr/share/zoneinfo")

# (code shown on the dial, IANA zone, city name for matching)
ZONES = [
    ("ET", "America/New_York", "Eastern Time"),
    ("CT", "America/Chicago", "Central Time"),
    ("MT", "America/Denver", "Mountain Time"),
    ("PT", "America/Los_Angeles", "Pacific Time"),
    ("NYC", "America/New_York", "New York"),
    ("BOS", "America/New_York", "Boston"),
    ("DC", "America/New_York", "Washington"),
    ("MIA", "America/New_York", "Miami"),
    ("ATL", "America/New_York", "Atlanta"),
    ("CHI", "America/Chicago", "Chicago"),
    ("DAL", "America/Chicago", "Dallas"),
    ("AUS", "America/Chicago", "Austin"),
    ("DEN", "America/Denver", "Denver"),
    ("PHX", "America/Phoenix", "Phoenix"),
    ("MPK", "America/Los_Angeles", "Menlo Park"),
    ("SFO", "America/Los_Angeles", "San Francisco"),
    ("SJC", "America/Los_Angeles", "San Jose"),
    ("LAX", "America/Los_Angeles", "Los Angeles"),
    ("SEA", "America/Los_Angeles", "Seattle"),
    ("ANC", "America/Anchorage", "Anchorage"),
    ("HNL", "Pacific/Honolulu", "Honolulu"),
    ("YVR", "America/Vancouver", "Vancouver"),
    ("YYZ", "America/Toronto", "Toronto"),
    ("MEX", "America/Mexico_City", "Mexico City"),
    ("BOG", "America/Bogota", "Bogota"),
    ("LIM", "America/Lima", "Lima"),
    ("SCL", "America/Santiago", "Santiago"),
    ("SAO", "America/Sao_Paulo", "Sao Paulo"),
    ("RIO", "America/Sao_Paulo", "Rio de Janeiro"),
    ("BUE", "America/Argentina/Buenos_Aires", "Buenos Aires"),
    ("LON", "Europe/London", "London"),
    ("DUB", "Europe/Dublin", "Dublin"),
    ("LIS", "Europe/Lisbon", "Lisbon"),
    ("PAR", "Europe/Paris", "Paris"),
    ("MAD", "Europe/Madrid", "Madrid"),
    ("BCN", "Europe/Madrid", "Barcelona"),
    ("AMS", "Europe/Amsterdam", "Amsterdam"),
    ("BRU", "Europe/Brussels", "Brussels"),
    ("BER", "Europe/Berlin", "Berlin"),
    ("MUC", "Europe/Berlin", "Munich"),
    ("ZRH", "Europe/Zurich", "Zurich"),
    ("GVA", "Europe/Zurich", "Geneva"),
    ("ROM", "Europe/Rome", "Rome"),
    ("MIL", "Europe/Rome", "Milan"),
    ("VIE", "Europe/Vienna", "Vienna"),
    ("PRG", "Europe/Prague", "Prague"),
    ("WAW", "Europe/Warsaw", "Warsaw"),
    ("CPH", "Europe/Copenhagen", "Copenhagen"),
    ("STO", "Europe/Stockholm", "Stockholm"),
    ("OSL", "Europe/Oslo", "Oslo"),
    ("HEL", "Europe/Helsinki", "Helsinki"),
    ("ATH", "Europe/Athens", "Athens"),
    ("IST", "Europe/Istanbul", "Istanbul"),
    ("MOW", "Europe/Moscow", "Moscow"),
    ("TLV", "Asia/Jerusalem", "Tel Aviv"),
    ("JRS", "Asia/Jerusalem", "Jerusalem"),
    ("CAI", "Africa/Cairo", "Cairo"),
    ("JNB", "Africa/Johannesburg", "Johannesburg"),
    ("CPT", "Africa/Johannesburg", "Cape Town"),
    ("LOS", "Africa/Lagos", "Lagos"),
    ("NBO", "Africa/Nairobi", "Nairobi"),
    ("DXB", "Asia/Dubai", "Dubai"),
    ("DOH", "Asia/Qatar", "Doha"),
    ("RUH", "Asia/Riyadh", "Riyadh"),
    ("DEL", "Asia/Kolkata", "Delhi"),
    ("BOM", "Asia/Kolkata", "Mumbai"),
    ("BLR", "Asia/Kolkata", "Bangalore"),
    ("SIN", "Asia/Singapore", "Singapore"),
    ("KUL", "Asia/Kuala_Lumpur", "Kuala Lumpur"),
    ("BKK", "Asia/Bangkok", "Bangkok"),
    ("JKT", "Asia/Jakarta", "Jakarta"),
    ("MNL", "Asia/Manila", "Manila"),
    ("HKG", "Asia/Hong_Kong", "Hong Kong"),
    ("SHA", "Asia/Shanghai", "Shanghai"),
    ("PEK", "Asia/Shanghai", "Beijing"),
    ("TPE", "Asia/Taipei", "Taipei"),
    ("SEL", "Asia/Seoul", "Seoul"),
    ("TYO", "Asia/Tokyo", "Tokyo"),
    ("SYD", "Australia/Sydney", "Sydney"),
    ("MEL", "Australia/Melbourne", "Melbourne"),
    ("BNE", "Australia/Brisbane", "Brisbane"),
    ("PER", "Australia/Perth", "Perth"),
    ("AKL", "Pacific/Auckland", "Auckland"),
    ("UTC", "Etc/UTC", "UTC"),
    ("GMT", "Etc/GMT", "Greenwich"),
]


def posix_tz(zone: str) -> str:
    data = (ZONEINFO / zone).read_bytes()
    if not data.startswith(b"TZif") or data[4:5] < b"2":
        raise SystemExit(f"{zone}: no TZif v2+ footer")
    footer = data.rstrip(b"\n").rsplit(b"\n", 1)[-1].decode("ascii")
    if not footer:
        raise SystemExit(f"{zone}: empty footer")
    return footer


def c_str(s: str) -> str:
    assert '"' not in s and "\\" not in s
    return f'"{s}"'


def main() -> None:
    out = Path(sys.argv[1])
    codes = set()
    rows = []
    for code, zone, name in ZONES:
        assert len(code) <= 4 and code not in codes, code
        codes.add(code)
        rows.append(f"    {{{c_str(code)}, {c_str(zone)}, {c_str(name)}, {c_str(posix_tz(zone))}}},")
    out.write_text(
        "/* Generated by firmware/tools/gen_zones.py from /usr/share/zoneinfo. Do not edit. */\n"
        '#include "muse_zones.h"\n\n'
        "const muse_zone_t muse_zones[] = {\n" + "\n".join(rows) + "\n};\n"
        "const int muse_zone_count = sizeof(muse_zones) / sizeof(muse_zones[0]);\n"
    )
    print(f"{out}: {len(rows)} zones")


if __name__ == "__main__":
    main()
