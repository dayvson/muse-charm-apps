My gadget (the round 466x466 screen) can now run Lua apps that you write, with no firmware update. Whenever I ask you to build an app for my gadget, follow these steps:

1. Read the API first.
Call luapp.admin with action "api". It returns the Lua API reference: screen widgets, timers, time, http, json, store and app. For anything with images, pixel art, animation or games, also read action "api_gfx" (gfx.image for PNG/JPEG, gfx.sprite for pixel art, ui.image, and ui.canvas for drawing frames with touch input). For sound, read action "api_sound" (sound.effect, sound.tone, sound.play for melodies). For richer widgets and motion, read action "api_ui" (ui.roller, ui.slider, ui.switch, ui.list, ui.scroll, ui.animate, ui.on_swipe, ui.on_button). For the rest of the gadget, read action "api_device" (muse.ask, battery and Wi-Fi, the microphone level, the motion sensor, alarms, the app's own files, HTTP POST, the built-in apps' data, screenshots). Every app needs a launcher icon: see "Launcher icon" in api_gfx. Use only what they describe. There is no io, os, require or load. Fonts are ASCII only, so no emoji.

2. Install with luapp.install:
- id: short and permanent, 1-12 characters of a-z, 0-9 and _ (for example "pomodoro"). Reusing an id updates that app.
- name: what the launcher shows, up to 16 plain ASCII characters (for example "Pomodoro").
- color: the launcher icon colour, "0xRRGGBB".
- code: the whole Lua source.
- Leave run at its default (true), so the board compiles, saves and opens the app on screen straight away.

3. If the code is over 8000 characters, send it in pieces with the same id:
- First piece: append false, done false
- Middle pieces: append true, done false
- Last piece: append true, done true (include name and color on it)

4. Check the reply. The app isn't deployed until you get an ok.
- ok with running true: it's installed and on screen. Tell me it's ready.
- syntax_error: comes with a line number. Fix the code and install again with the same id.
- runtime_error: it saved but crashed on start. Fix it and reinstall.
- full: 9 apps are installed already. Ask me which one to remove, then use luapp.admin action "remove".
- too_large: over 64 KB. Simplify the app.
- busy or unavailable: wait a moment and retry once. If that fails, tell me the gadget may be disconnected.

5. After I try it, call luapp.admin action "logs" to see print() output and any errors that happened later. Fix and reinstall if needed.

Other commands:
- luapp.admin action "list": installed apps, the one running, and whether storage is mounted.
- luapp.run with id: open an installed app.
- app.show with app "apps": show the launcher.
- luapp.admin action "stop": stop the running app.
- luapp.admin action "remove" with id: delete an app.
- luapp.admin action "source" with id and offset: read an app's code back, 3000 characters at a time, so you can edit it.

Icons: give every app its own icon by calling app.icon at start-up, so the launcher looks polished instead of showing initials. Best is a 96x96 ui.canvas with one bold, simple shape in 2-3 colours (no small text), passed to app.icon and then deleted. A pixel-art gfx.sprite (12, 18, 24 or 36 px square, transparent background) also works and sits on the app's colour. So do a PNG or JPEG photo, which fills the circle. When you update an older app, add an icon to it too.

Games: draw each frame on a ui.canvas from ui.every(16 to 33, ...), take input with on_touch, and use sound.effect for feedback. When small things move over a still scene, use c:partial(true) (see api_gfx) so each frame sends the screen only what changed, and check the speed with luapp.admin action "perf". Write original games, tunes and art.

Design rules:
- One idea per screen, big text, 2-3 colours.
- Keep content within about 200 px of the centre, because the screen is round.
- Each callback must finish within 250 ms.
- Save state with store.set so it survives restarts.
- For network data, use free APIs that need no key.
- I exit an app by long-pressing it (on a canvas with on_touch: holding still for 1.5 s).

Please remember these steps for future app requests.
