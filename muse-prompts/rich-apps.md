I updated my gadget's firmware: Lua apps can do a lot more now. The API is split into parts. Please read the two new ones before building anything new: luapp.admin action "api_ui" and action "api_device". Also reread "api" and "api_gfx", which changed. Here's what's new:

- More widgets (api_ui): ui.roller (a spinning picker), ui.slider, ui.switch, ui.list (scrolling rows with on_select), and ui.scroll (a scrolling area: create widgets with parent=<scroll>).
- Animation (api_ui): ui.animate(widget, {x, y, opacity, w, h, value, angle, scale}, ms, {ease, delay, repeat, reverse, on_done}). The screen runs the animation itself, so it's smooth and costs the app nothing. Use it for slides, fades and pulses instead of moving things in ui.every.
- Input (api_ui): ui.on_swipe(fn(dir)) for up and down swipes, and ui.on_button(fn) for the bottom button's short press while the app is on screen.
- Asking you (api_device): muse.ask(text, fn(answer, err)) sends you a typed message from the app and returns your text reply. When it comes from the gadget, answer briefly in plain text (or JSON when the app asks for it). It is shown on a small round screen.
- Talking to a running app: luapp.send {data = "JSON text"} calls the app's app.on_message(data) and returns what the handler returns. Use it to push content into an app or to read its state.
- The board (api_device): device.info() (battery, charging, Wi-Fi, Muse status, call, volume, brightness), app.keep_awake(true), app.brightness(pct), mic.level() (loudness in dBFS), motion.read() (accelerometer and gyroscope; learn the axes with a small test app first).
- Alarms (api_device): app.alarm(seconds or unix time, tag) and app.on_alarm(fn(tag)). At that time the gadget wakes, opens the app and calls the handler, even if it's asleep or showing another app. Alarms survive restarts.
- Files (api_device): fs.write/append/read/remove/list, 256 KB per app. To cache an image: fs.write("x.png", body), then later gfx.image(fs.read("x.png")).
- Data from my built-in apps (api_device): apps.stocks(), apps.wind() and apps.watch().
- HTTP (api_device): http.post(url, body, fn, headers) and http.request{url, method, headers, body}, fn. A table body is sent as JSON.
- Faster canvas drawing (api_gfx): c:polygon, c:gradient, c:tilemap (a whole tile map in one call) and c:sprites (one image at many positions in one call). There's also a note on redrawing only what changed.
- Seeing an app: luapp.admin action "screenshot" returns the screen as a PNG in base64 pages. Start with offset 0, then pass offset = next until next is null, and join the pieces. full=true gives 466 px instead of 233 px. Action "widgets" lists the running app's widgets as JSON, which is cheaper for checking a layout. After installing or changing an app, please check it with widgets or a screenshot before telling me it's done.

To try it out, please build a small "Lab" app (id "lab") that exercises the new pieces:
- A roller to pick a page.
- A page with device.info() and mic.level() as a live bar.
- A page with motion.read() values.
- A button that runs muse.ask("Say hi in five words", ...) and shows the answer.
- A 1-minute app.alarm test.
- ui.on_button to cycle pages.

Then take a screenshot and tell me what you see, and what motion.read() reports with the gadget lying flat.
