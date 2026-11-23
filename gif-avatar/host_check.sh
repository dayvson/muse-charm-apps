#!/bin/sh
# Host checks for a generated muse_pixel.c, as tools/muse/avatar.py host_check() does them:
# -O1 -g -Wall -Werror against tools/muse/anim.c, then an ASan/UBSan run of every animation.
# Extra: -Wextra, and the exported-symbol list. Usage: host_check.sh path/to/muse_pixel.c
set -e
SRC=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
ROOT=$(cd "$(dirname "$0")/.." && pwd)
ESP32=${MUSE_ESP32_DIR:-$ROOT/muse-gadget-sdk/esp32}
TMP=$(mktemp -d /tmp/gifavatar.XXXXXX)
trap 'rm -rf "$TMP"' EXIT
cd "$ESP32"
# Frames in a flash partition (gif2pixel --external): the host build reads the .bin.
if grep -q '^#define GIF_EXTERNAL 1' "$SRC"; then
    export MUSE_PIXEL_BIN="${SRC%.c}.bin"
    [ -f "$MUSE_PIXEL_BIN" ] || { echo "no $MUSE_PIXEL_BIN beside $SRC"; exit 1; }
fi
CC="cc -O1 -g -Wall -Werror -I components/muse tools/muse/anim.c $SRC -lm"
echo "[1] cc -O1 -g -Wall -Werror (avatar.py's flags)"
$CC -o "$TMP/anim"
echo "[2] cc -O2 -Wall -Wextra -Werror"
cc -O2 -Wall -Wextra -Werror -I components/muse -c "$SRC" -o "$TMP/mp.o"
echo "[3] ASan/UBSan run of every animation"
$CC -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -o "$TMP/anim_san"
mkdir "$TMP/frames"
"$TMP/anim_san" "$TMP/frames"
echo "    $(ls "$TMP/frames" | wc -l | tr -d ' ') animations, $(find "$TMP/frames" -name '*.ppm' | wc -l | tr -d ' ') frames, no sanitizer errors"
echo "[4] exported symbols:"
nm -g "$TMP/mp.o" | grep -v ' U ' | awk '{print "    " $3}' | sed 's/^    _/    /'
echo "    undefined: $(nm -u "$TMP/mp.o" | sed 's/^_//' | tr '\n' ' ')"
echo "[5] avatar.py API/description checks"
for f in muse_pixel_accent muse_pixel_render muse_pixel_set_size muse_pixel_scale muse_pixel.h; do
    grep -q "$f" "$SRC" || { echo "missing $f"; exit 1; }
done
"$ROOT/toolchain/tools-venv/bin/python" -c "
import sys; sys.path.insert(0, 'tools/muse')
import re
src = open('$SRC').read()
# avatar.py's description(), copied (importing avatar.py pulls in serial/chat)
m = re.match(r'\s*(?:/\*.*?(?:Copyright|License).*?\*/|//[^\n]*Copyright[^\n]*\n)\s*(/\*.*?\*/|(?://[^\n]*\n\s*)+)', src, re.S)
assert m, 'no description comment below the copyright line'
lines = (re.sub(r'^\s*(/\*+|\*+/|\*|//)|\*+/\s*$', '', l) for l in m.group(1).splitlines())
print('    description:', ' '.join(' '.join(lines).split())[:160], '...')
"
echo "OK"
