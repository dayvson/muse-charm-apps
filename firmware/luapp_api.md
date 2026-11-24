# Lua apps for the Muse gadget

Lua 5.4 apps on a 466x466 round touch screen (an ESP32-S3 watch-sized gadget).
Install with `luapp.install` (id, name, color, code); it compiles, saves and runs
the app, and returns syntax or start-up errors with line numbers. Errors later
are in `luapp.admin` action `logs`. Up to 64 apps, 64 KB of source each.
More: `api_ui` (rollers, sliders, switches, lists, scrolling, animation,
swipes, the button), `api_gfx` (images, sprites, canvas), `api_sound`,
`api_device` (muse.ask, battery, motion, mic, alarms, files, HTTP POST,
the built-in apps' data, screenshots).

## How an app runs

The script runs once, top to bottom, to build the screen. After that only its
callbacks run: taps, timers and HTTP replies. Keep each run short: 250 ms (the
first run 1 s). The app gets 384 KB of memory. Any error stops the app and shows
on screen. The user leaves with a long-press; swiping sideways changes screens.
Timers pause while the app is off screen or the screen sleeps. 30 s off the
Apps screen (or a long-press, or luapp.admin stop) unloads it: memory,
graphics and timers are freed; its store is kept and it starts fresh next time.
Frames are paced: a late one is skipped, not caught up.
Updating a running app just works: pieces upload while it runs, and the last
piece stops it at once. `stop` answers once it's unloaded (what it freed); if
an app won't stop in 3 s, `stop` with force=true reboots the gadget (~10 s).

Sandbox: base Lua (no load, dofile, require, collectgarbage), string, table,
math, utf8, coroutine, plus the modules below and gfx and sound. No io, os
or debug.

## Screen

Coordinates are a widget's centre, from the screen's centre: +x right, +y down.
The visible area is a circle of radius 233, so keep things within about 200 of
the centre, nearer at the top and bottom. Colours are 0xRRGGBB. Text sizes:
14, 16, 20, 28, 48 (others round to these). Fonts are ASCII plus degree sign
and bullet: no emoji or accented letters.

    ui.label{text, x, y, size=16, color=0xFFFFFF, w=nil, align="center"}
      -- w: wrap width; align: "left" | "center" | "right" within w
    ui.box{x, y, w=100, h=100, color=0x1C1C1E, radius=0, border=0, border_color, opacity=255}
    ui.button{x, y, w=140, h=48, text, size=16, color=0x2C2C2E, text_color=0xFFFFFF,
              radius=h/2, on_tap=function() end}
    ui.arc{x, y, r=40, width=8, min=0, max=100, value=0, start=0, sweep=360,
           color=0x27D3A2, bg=0x2C2C2E}       -- start: degrees clockwise from the top
    ui.bar{x, y, w=200, h=12, min=0, max=100, value=0, color=0x27D3A2, bg=0x2C2C2E, radius}
    ui.line{points={x1,y1, x2,y2, ...}, color=0xFFFFFF, width=2}   -- 2 to 128 points

Each returns a widget: `w:set{...}` changes any of its fields (text, value,
color, points, on_tap, ...), `w:hide()`, `w:show()`, `w:delete()`. Numbers are
whole (arc and bar values too: scale them). Up to 96 widgets.
ui.image and ui.canvas are widgets too (see api_gfx).

    ui.clear()                     -- delete every widget
    ui.background(0x000000)
    ui.on_tap(function(x, y) end)  -- taps outside buttons, centre-relative
    id = ui.every(ms, fn)          -- repeating timer (ms >= 16; games 16-33); up to 24
    id = ui.after(ms, fn)          -- once
    ui.cancel(id)

## Time

    time.now()      -- Unix time, seconds (with fraction)
    time.synced()   -- false until the clock is set from the network
    time.date(t)    -- {year, month, day, hour, min, sec, wday (1 = Sunday), utc_offset}
                    -- in the user's local time zone (the watch's); t defaults to now
    time.utc(t)     -- the same, in UTC

## Network

    http.get(url, function(ok, body, status) end)

GET over https (or http); POST, headers and more in api_device. The body is a string up to 32 KB. Requests run one
at a time (up to 4 queued) and wait while a voice call is on. ok is false with
a reason in body when it fails. Use APIs without keys (Open-Meteo, for example).

    json.decode(s)  -- null becomes nil
    json.encode(v)

## Storage

    store.get(key)          -- nil if unset
    store.set(key, value)   -- strings, numbers, booleans; kept across restarts

## Other

    app.id, app.name
    app.icon(img)           -- launcher icon: an image or canvas (api_gfx)
    app.exit()              -- back to the launcher
    print(...)              -- to luapp.admin logs

## Example

    local n = store.get("count") or 0
    ui.label{text="TAPS", y=-90, size=16, color=0x8E8E93}
    local count = ui.label{text=tostring(n), y=-30, size=48}
    local ring = ui.arc{y=-30, r=95, width=10, max=50, value=n % 50, color=0xFF6B6B}
    ui.button{text="+1", y=110, w=120, color=0xFF6B6B, text_color=0x000000,
      on_tap=function()
        n = n + 1
        store.set("count", n)
        count:set{text=tostring(n)}
        ring:set{value=n % 50}
      end}

Design for the round screen: one idea per screen, big text, 2-3 colours,
generous spacing. Give every app an icon with app.icon; without one the
launcher shows its initials on its `color`.
