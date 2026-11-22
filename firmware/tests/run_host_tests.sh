#!/bin/sh
# Host tests for the apps: timezone engine vs zoneinfo, watch logic, dial decoder.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
FW="$HERE/.."
PY="${PY:-$HERE/../../toolchain/tools-venv/bin/python}"
CJ="$(ls -d "$HOME"/Library/Caches/Espressif/ComponentManager/*/espressif__cjson_*/cJSON | tail -1)"
T="$(mktemp -d)"
"$PY" -I "$HERE/test_tz.py"
if ls "$HERE"/fixtures/META_1d.json >/dev/null 2>&1; then "$PY" -I "$HERE/test_stocks.py"; else echo "stocks: no fixtures (tests/fetch_fixtures.sh), skipped"; fi
if ls "$HERE"/fixtures/wind_*.json >/dev/null 2>&1; then "$PY" -I "$HERE/test_wind.py"; else echo "wind: no fixtures (tests/fetch_fixtures.sh), skipped"; fi
SAN="-fsanitize=address,undefined"
cc -std=c11 -O1 $SAN -w -I"$CJ" -c "$CJ/cJSON.c" -o "$T/cJSON.o"
cc -std=c11 -Wall -Wextra -Werror -O1 $SAN -I"$FW" -I"$CJ" "$HERE/test_watch_core.c" \
   "$FW/watch_core.c" "$FW/watch_dial.c" "$FW/watch_assets.c" "$FW/muse_tz.c" "$FW/muse_zones.c" \
   "$FW/muse_zones_find.c" "$T/cJSON.o" -o "$T/twc"
"$T/twc" "$T/dial.ppm"
"$PY" -I - "$T/dial.ppm" "$HERE/../preview/dial-quantized.png" <<'PY'
import sys
from PIL import Image
a = Image.open(sys.argv[1]).convert("RGB")
b = Image.open(sys.argv[2]).convert("RGB")
# the board's dial is RGB565: compare at that precision
d = max(abs((x >> s) - (y >> s)) for p, q in zip(a.get_flattened_data(), b.get_flattened_data())
        for x, y, s in zip(p, q, (3, 2, 3)))
print("dial decoded on the host matches the render" if d == 0 else f"dial MISMATCH (max {d})")
sys.exit(d != 0)
PY

# Lua apps: the sandbox and json module, on Lua 5.4.7 built for the host.
mkdir -p "$T/lua"
for f in "$FW"/lua/*.c; do cc -std=gnu99 -O1 $SAN -w -I"$FW/lua" -c "$f" -o "$T/lua/$(basename "$f" .c).o"; done
cc -std=c11 -Wall -Wextra -Werror -O1 $SAN -I"$FW" -I"$FW/lua" "$HERE/test_luapp.c" "$FW/luapp_sandbox.c" \
   "$FW/luapp_json.c" "$T"/lua/*.o -lm -o "$T/tl"
"$T/tl"

# Lua apps: images (PNG decoder, sprites, base64) and the sound synth.
cc -std=c11 -Wall -Wextra -Werror -O1 $SAN -I"$FW" "$HERE/gfx_cli.c" "$FW/luapp_image.c" "$FW/luapp_synth.c" \
   -lz -lm -o "$T/gfx_cli"
"$PY" -I "$HERE/test_gfx.py" "$T/gfx_cli"
