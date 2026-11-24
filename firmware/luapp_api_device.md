# The gadget, Muse and the network

## Asking Muse

    muse.ask(text, function(answer, err) end)   -- answer: Muse's reply, or nil and err
    muse.status()   -- "connected", "connecting", "offline", "unreachable", "not set up"

Sends text to Muse as a typed message (marked as coming from this app) and
returns Muse's text reply, usually in 5-30 s. One at a time, 5 s apart, 30
per run; nothing while a voice call is on. Asking while one is on its way
queues the new question (only the newest: one it replaces gets nil,
"replaced by a newer question"). For anything a web API can answer, http
is far faster: a turn with Muse takes seconds. Ask for a short plain-text answer,
or JSON you can json.decode. The user's chat history shows the exchange.

    app.on_message(function(data) return reply end)

Muse can talk to the running app with `luapp.send` {data = JSON text}: data
arrives decoded (a table, string or number), and what the handler returns
goes back to Muse as JSON. Useful for pushing content or querying state.

## The board

    device.info()   -- {battery (%, nil without one), battery_mv, charging, usb,
                    --  wifi, muse (as muse.status), call, volume, speaker,
                    --  brightness, uptime (s), psram_kb}
    device.battery() -- percent (or nil), charging
    app.keep_awake(true)   -- the screen doesn't sleep while the app is on it
    app.brightness(pct)    -- 10-100 while the app is on screen; nil restores
    mic.level()     -- microphone loudness in dBFS (-100 silent .. 0 loudest),
                    -- smoothed; -100 during a voice call
    motion.read()   -- ax, ay, az (g), gx, gy, gz (degrees/s), or nil, err
    motion.available()

motion reads the board's accelerometer and gyroscope (the sensor starts on first
use). Lying flat, one axis reads about 1 g; try it to learn the axes. For
shakes, read it in ui.every(50) and look for |a| well above 1.5 g.

## Alarms

    app.alarm(when, tag="alarm")   -- when: seconds from now, or a Unix time; up to 30 days
    app.on_alarm(function(tag) end)
    app.cancel_alarm(tag)          -- no tag: all of the app's
    app.alarms()                   -- {tag = unix_time, ...}

At the time, even if another app is open or the screen sleeps, the gadget wakes,
opens your app (starting it fresh if it isn't running) and calls on_alarm(tag).
Set on_alarm when the script starts. 4 per app; kept across restarts; setting
a tag again moves it. Needs the clock (time.synced()). Not during a call (it
fires after). Play a sound in on_alarm (api_sound) to be noticed.

## Files

    fs.write(name, s)    -- true, or false and why
    fs.append(name, s)
    fs.read(name)        -- the string, or nil
    fs.remove(name)
    fs.list()            -- {name = bytes, ...}, used, quota

The app's own files, for data bigger than store (8 KB): names of a-z 0-9 _ - .,
up to 128 KB each, 256 KB per app. Removed with the app. To cache a picture:
fs.write("map.png", body) from http, then gfx.image(fs.read("map.png")).
Writing flash is slow (tens of ms): not every frame.

## Built-in apps

    apps.stocks()   -- {quotes = {{symbol, price, previous_close, change_pct, as_of}, ...}}
    apps.wind()     -- {spots = {...}}: each kite spot's wind now and best windows
    apps.watch()    -- the dual-time watch's zones and times

The same data as the stocks.get, wind.get and watch.get commands.

## More HTTP

    http.get(url, fn, headers)
    http.post(url, body, fn, headers)
    http.request({url=, method="GET", headers={...}, body=}, fn)

fn is called as fn(ok, body, status), as with http.get. method: GET, POST,
PUT, PATCH, DELETE or HEAD. headers: {["Name"] = "value"}. body: a string,
or a table sent as JSON. Bodies up to 16 KB out, 32 KB back. Requests run one
at a time for all apps; the connection stays open 20 s, so repeated calls to
one host (polling an API every few seconds) skip the TLS handshake.

## Seeing the app (for Muse)

`luapp.admin` action `screenshot` returns the screen as a PNG in base64
pages: call with offset 0 (full=true for 466 px instead of 233), then with
offset = next until next is null, and join the png_base64 pieces. Action
`widgets` lists the running app's widgets (kind, position, size, text,
value) as JSON. It's cheaper for checking layout.
Action `perf` gives the last second's speed (as gfx.stats: screen fps, ms
and KB per frame, the app's Lua and drawing time) without touching the app.
