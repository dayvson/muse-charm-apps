#!/usr/bin/env bash
# Choose the avatar the next firmware build shows: the SDK's default character,
# or your own Muse avatar made from the animation clips Muse exports.
#
#   scripts/05_avatar.sh status                 which avatar the next build uses
#   scripts/05_avatar.sh default                switch to the SDK's default avatar
#   scripts/05_avatar.sh ask [--call-clips] [--print]
#                                               ask Muse (through the board) to zip its
#                                               avatar clips and attach them in the Muse app;
#                                               --call-clips asks for speaking/listening clips;
#                                               --print only prints the prompt to paste yourself
#   scripts/05_avatar.sh make ZIP|DIR... [--id ID] [--name TEXT] [--budget-kb N] [-- gif2pixel options]
#                                               turn the clips into avatars/ID.c and use it
#   scripts/05_avatar.sh use ID|FILE            switch to a saved avatar (avatars/ID.c) or any muse_pixel.c
#   scripts/05_avatar.sh list                   saved avatars
#
# Nothing here touches the board: rebuild (scripts/02_build.sh) and flash
# (scripts/04_flash_muse.sh --reflash) to see the change.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PY="$ROOT/toolchain/tools-venv/bin/python"
AVATARS="$ROOT/avatars"                       # your generated avatars (not committed)
CLIPS="$ROOT/avatar-clips"                    # extracted clips (not committed)
TOOLS="$ROOT/gif-avatar"
SDK_AVATAR_DIR="$ROOT/muse-gadget-sdk/esp32/components/muse/avatar"
ACTIVE="$SDK_AVATAR_DIR/muse_pixel.c"         # the SDK builds this in place of its default when it exists

usage() { sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-1}"; }
die() { echo "05_avatar: $*" >&2; exit 1; }
sha() { shasum -a 256 "$1" | cut -c1-64; }
next_steps() { echo; echo "Next: scripts/02_build.sh, then scripts/04_flash_muse.sh --reflash"; }

saved_name() {  # the avatars/*.c with the same content as $1, if any
  local f
  for f in "$AVATARS"/*.c; do
    [ -f "$f" ] && [ "$(sha "$f")" = "$(sha "$1")" ] && { basename "$f" .c; return; }
  done
  return 0
}

cmd_status() {
  if [ -f "$ACTIVE" ]; then
    local n; n="$(saved_name "$ACTIVE")"
    echo "Next build uses: your avatar${n:+ \"$n\" (avatars/$n.c)} ($(du -k "$ACTIVE" | cut -f1) KB source)"
    grep -m1 -E '^ \* .*: a clip-backed avatar' "$ACTIVE" | sed 's/^ \* /  /' || true
  else
    echo "Next build uses: the SDK's default avatar"
  fi
}

cmd_default() {
  if [ -f "$ACTIVE" ]; then
    [ -n "$(saved_name "$ACTIVE")" ] || { mkdir -p "$AVATARS"; cp "$ACTIVE" "$AVATARS/unsaved-$(date +%Y%m%d-%H%M%S).c"; echo "Kept a copy of the current one in avatars/."; }
    mv "$ACTIVE" "$ACTIVE.prev"
  fi
  echo "The next build uses the SDK's default avatar."
  next_steps
}

cmd_use() {
  [ $# -eq 1 ] || usage
  local src="$1"
  [ -f "$src" ] || src="$AVATARS/$1.c"
  [ -f "$src" ] || die "no avatar '$1' (see: scripts/05_avatar.sh list)"
  grep -q 'muse_pixel_render' "$src" || die "$src doesn't look like a muse_pixel.c"
  mkdir -p "$SDK_AVATAR_DIR"
  [ -f "$ACTIVE" ] && cp "$ACTIVE" "$ACTIVE.prev"
  cp "$src" "$ACTIVE"
  echo "The next build uses $src."
  next_steps
}

cmd_list() {
  local f any=
  for f in "$AVATARS"/*.c; do
    [ -f "$f" ] || continue
    any=1
    printf '  %-28s %5s KB  %s\n' "$(basename "$f" .c)" "$(du -k "$f" | cut -f1)" "$(date -r "$f" '+%Y-%m-%d %H:%M')"
  done
  [ -n "$any" ] || echo "  (none yet: scripts/05_avatar.sh make ...)"
  cmd_status
}

cmd_ask() {
  local prompt="$TOOLS/prompts/01_export_clips.md" print= zipname=avatar-clips.zip
  while [ $# -gt 0 ]; do
    case "$1" in
      --call-clips) prompt="$TOOLS/prompts/02_call_clips.md"; zipname=call-clips.zip ;;
      --print) print=1 ;;
      *) usage ;;
    esac
    shift
  done
  if [ -n "$print" ]; then
    cat "$prompt"
    echo; echo "---- paste the text above into your chat with Muse in the Muse app"
  else
    echo "==> Asking Muse through the board (paste gif-avatar/prompts/$(basename "$prompt") into the Muse app instead if this fails)"
    "$ROOT/scripts/06_ask_muse.sh" --file "$prompt" || echo "(No reply through the board; Muse may still be working. Check the Muse app.)"
  fi
  cat <<EOF

When Muse attaches $zipname in the Muse app, save it from your phone to this
Mac (AirDrop works), then run:
  scripts/05_avatar.sh make ~/Downloads/$zipname --id my-avatar
(add the call clips zip too, if you asked for it: make ZIP1 ZIP2 ...)
EOF
}

cmd_make() {
  local id=my-avatar name="Muse avatar" budget=512 srcs=() extra=()
  while [ $# -gt 0 ]; do
    case "$1" in
      --id) id="$2"; shift ;;
      --name) name="$2"; shift ;;
      --budget-kb) budget="$2"; shift ;;
      --) shift; extra=("$@"); break ;;
      -h|--help) usage 0 ;;
      *) srcs+=("$1") ;;
    esac
    shift
  done
  [ ${#srcs[@]} -gt 0 ] || usage
  [[ "$id" =~ ^[A-Za-z0-9_-]{1,40}$ ]] || die "--id: letters, digits, - and _ only"
  [ -x "$PY" ] || die "no Python at $PY (run scripts/00_setup.sh)"

  local dir="$CLIPS/$id-$(date +%Y%m%d-%H%M%S)"
  mkdir -p "$dir"
  echo "==> Collecting clips into ${dir#$ROOT/}"
  # The zips come from outside: take only media files, by base name, and cap the size.
  "$PY" -I - "$dir" "${srcs[@]}" <<'EOF'
import os, shutil, sys, zipfile
out, srcs = sys.argv[1], sys.argv[2:]
MEDIA = (".mp4", ".mov", ".m4v", ".webm", ".gif", ".webp", ".png")
LIMIT = 300 << 20
total = 0
def take(name, opener, size):
    global total
    base = os.path.basename(name.replace("\\", "/"))
    if not base or base.startswith(".") or not base.lower().endswith(MEDIA):
        return
    total += size
    if total > LIMIT:
        sys.exit("clips add up to more than 300 MB; is this the right archive?")
    with opener() as src, open(os.path.join(out, base), "wb") as dst:
        shutil.copyfileobj(src, dst)
    print(f"   {base} ({size // 1024} KB)")
for s in srcs:
    s = os.path.expanduser(s)
    if os.path.isdir(s):
        for n in sorted(os.listdir(s)):
            p = os.path.join(s, n)
            if os.path.isfile(p):
                take(n, lambda p=p: open(p, "rb"), os.path.getsize(p))
    elif zipfile.is_zipfile(s):
        with zipfile.ZipFile(s) as z:
            for i in z.infolist():
                if not i.is_dir():
                    take(i.filename, lambda i=i: z.open(i), i.file_size)
    else:
        sys.exit(f"{s}: not a zip or a folder")
EOF

  mkdir -p "$AVATARS"
  local out="$AVATARS/$id.c" tmp="$AVATARS/.$id.tmp.c" prev="$AVATARS/$id-preview"
  echo "==> Converting (frame data budget ${budget} KB)"
  "$PY" -I "$TOOLS/gif2pixel.py" "$dir" -o "$tmp" \
      --name "$name" \
      --crop full --margin 0 --clip-config "$TOOLS/clips.json" \
      --speak bounce --speak-bounce 1 --speak-pulse 0.12 \
      --budget-kb "$budget" --dump "$prev" ${extra[@]+"${extra[@]}"}
  echo "==> Checking the generated C on this Mac"
  "$TOOLS/host_check.sh" "$tmp" | tail -3
  mv "$tmp" "$out"
  echo "Saved avatars/$id.c ($(du -k "$out" | cut -f1) KB source); previews (5x GIFs) in avatars/$id-preview/"
  cmd_use "$out"
}

[ $# -ge 1 ] || usage
sub="$1"; shift
case "$sub" in
  status) cmd_status ;;
  default) cmd_default ;;
  use) cmd_use "$@" ;;
  list) cmd_list ;;
  ask) cmd_ask "$@" ;;
  make) cmd_make "$@" ;;
  -h|--help|help) usage 0 ;;
  *) usage ;;
esac
