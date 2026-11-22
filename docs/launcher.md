# The Lua app launcher: apps Muse writes and deploys

The **Apps** screen, one swipe left of Muse, is a launcher for apps written in
Lua. Muse writes them and installs them over its existing link to the board. The
board compiles, saves and opens each app on the spot, with **no firmware build
and no flashing**. You ask for an app in a sentence, and a minute later it's on
your desk.

![Launcher](../firmware/preview/launcher-sheet.png)

## How it works

```
 you: "build me a pomodoro timer"
        |
     Muse (cloud) --- reads the API from the board:  luapp.admin {action: "api"}
        |              writes Lua, then sends:       luapp.install {id, name, color, code}
        v
     the board ------ compiles it (syntax errors go back with line numbers)
                      saves it to the luapps partition (FAT, 1 MB)
                      opens it; start-up errors go back too
                      -> "ok, running"
```

- **Runtime:** stock Lua 5.4.7 ([`firmware/lua/`](../firmware/lua/), unmodified)
  embedded in the firmware ([`luapp_vm.c`](../firmware/luapp_vm.c)). One app runs at
  a time on its own task: a 64 KB stack in PSRAM, a 384 KB Lua heap, and up to 1.5 MB
  for images and canvases. The board always keeps 512 KB free for Muse's voice.
- **Sandbox** ([`luapp_sandbox.c`](../firmware/luapp_sandbox.c)): base Lua without
  `load`, `dofile`, `require` or `collectgarbage`, plus string, table, math, utf8 and
  coroutine. There is no `io`, `os` or `debug`. Apps reach the outside world only
  through the modules below.
- **Time limits:** each callback has 250 ms (the first run of the script gets 1 s). A
  hook stops a runaway callback, and an error stops the app and shows on screen.
- **Storage:** up to 9 apps of up to 64 KB of source each, plus each app's `store`
  values, on the `luapps` FAT partition
  ([`config/partitions-muse-charm.csv`](../config/partitions-muse-charm.csv)). Apps
  survive reflashing, because the partition table keeps NVS and the app slots where
  the SDK has them.
- **Launcher:** pages of six (two rows of three 96 px round icons), one page per
  vertical swipe, with page dots. Each app draws its own icon at start-up with
  `app.icon`, saved next to the app and cached for the launcher.

## What an app can use

The full reference lives on the board: Muse reads it with `luapp.admin` and actions
`api`, `api_gfx` and `api_sound`. The source files are
[`luapp_api.md`](../firmware/luapp_api.md),
[`luapp_api_gfx.md`](../firmware/luapp_api_gfx.md) and
[`luapp_api_sound.md`](../firmware/luapp_api_sound.md), compiled into the firmware by
[`tools/embed_text.py`](../firmware/tools/embed_text.py). After editing them, run
`embed_text.py` (the command is in its header) and rebuild.

| Module | What |
|---|---|
| `ui` | label, box, button, arc, bar, line, image, canvas (up to 96 widgets); taps; `ui.every` / `ui.after` timers (up to 24); background |
| `time` | `now`, `date`, `utc`, `synced` |
| `http`, `json` | `http.get(url, callback)`: HTTP(S) GET, bodies up to 32 KB, one request at a time, paused during voice calls; `json.decode` / `encode` |
| `store` | per-app key/value storage that survives restarts |
| `gfx` | PNG and JPEG images (decoded with the ESP32-S3 ROM's tinfl and TJpgDec), pixel-art sprites, a canvas with drawing ops and touch input, zoomed canvases (2-4x, crisp), `gfx.memory()` |
| `sound` | a 4-channel synth: effects, tones, and melodies in note strings (`"C4:8 E G C5:4"`), mixed into Muse's speaker when Muse isn't talking |
| `app` | `id`, `name`, `icon(img)`, `exit()` |

The screen is round: coordinates start at the centre (+x right, +y down), and
content should stay within about 200 px of it. Fonts are ASCII, plus the degree
sign and bullet.

## Muse's commands

| Command | Does |
|---|---|
| `luapp.install` | `id`, `code`, and optionally `name`, `color`, `run`. Installs or updates an app; code over 8000 characters goes in pieces (`done=false`, then `append=true`). Returns ok, or `syntax_error` / `runtime_error` with a line number, or `full`, `too_large`, `busy`. |
| `luapp.run` | Opens an installed app; returns its start-up error, if any |
| `luapp.admin` | `action`: `api`, `api_gfx`, `api_sound` (the reference), `list` (apps, the running one, storage, the last unload), `logs` (the app's `print` output and later errors), `stop` (answers once it's unloaded; `force=true` reboots the board if the app won't stop in 3 s), `remove`, `source` (read code back in 3000-character pieces, for editing) |
| `app.show` | `app: "apps"` shows the launcher |

## Lifecycle

- **Opening:** tap an icon, or Muse calls `luapp.run` or installs with `run=true`.
- **Leaving:** a long-press exits back to the launcher (on a canvas with touch, hold
  still for 1.5 s). Swiping sideways changes screens: the app keeps running with its
  timers paused, and it's unloaded after 30 s off the Apps screen.
- **Updating a running app:** pieces upload while it runs, and the last piece stops
  and replaces it.
- **Unloading** frees the app's memory, graphics and timers. Its `store` is kept, and
  it starts fresh next time.
- Muse's commands are handled before the app's timers, so a busy animation can't
  starve Muse's link.

## Teaching Muse

Muse doesn't know about the launcher until you tell it. Send it
[`muse-prompts/lua-apps.md`](../muse-prompts/lua-apps.md) once, in the Muse app or
through the board:

```sh
scripts/06_ask_muse.sh --file muse-prompts/lua-apps.md
```

It covers reading the API first, choosing an id, sending large code in pieces,
handling each error reply, checking logs after you try the app, icons, and design
rules for the round screen. The device guide
([`config/device-guide.md`](../config/device-guide.md)) carries a short version, so
Muse knows the launcher exists in every conversation.

## Installing the apps in this repository

Each folder in [`apps/`](../apps/) has the app's Lua source and a README with its
id, name and colour. To put one on your board, give Muse the source (paste it, or
point Muse at this repository) and ask:

> Install this Lua app on my gadget with luapp.install: id `timer`, name "Timer",
> colour 0x34C759. Send it in pieces if it's over 8000 characters.

To save an app Muse wrote, ask Muse for it (it can read it back with `luapp.admin`
`source`) and commit it under `apps/<id>/` with a README.

## Limits worth knowing

- One app at a time. Opening another unloads the first.
- `link.register` is capped at 8 KB including the SDK's own commands, so these
  commands' descriptions are short. The detail lives in `luapp.admin api`.
- Each API reply is kept to about 4 KB, which is why it's split into three actions.
- The flash app slot is about 90% full with Lua, the native apps and a custom avatar.
- Internal RAM is tight during voice calls ([memory.md](memory.md)). Lua runs from
  PSRAM, but avoid HTTP fetches in a tight loop.
