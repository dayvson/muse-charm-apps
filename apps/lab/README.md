# Lab

A test bench for the November 2026 firmware APIs, built to try
each new piece once: widgets and input from api_ui, and the
board, Muse, motion, mic and alarm calls from api_device.

A roller at the top picks the page, and a short press of the
bottom button cycles pages too (ui.on_button, with the roller
kept in sync). Three pages:

- Device: device.info() readouts (battery, Wi-Fi, Muse
  status, volume, uptime) refreshed every second, and a live
  mic.level() bar updated four times a second. A button sets
  a 1-minute app.alarm; when it fires the gadget wakes, opens
  Lab and shows ALARM FIRED with a sound, even if another app
  was open.
- Motion: motion.read() accelerometer (g) and gyroscope
  (deg/s) values, live. Lying flat, AZ reads about +0.93 g
  and the other axes sit near zero.
- Ask Muse: a button sends muse.ask("Say hi in five
  words") and shows the reply on screen.

The app also answers luapp.send through app.on_message with
its page, battery, Muse status and a fresh motion.read(),
which is how the values above were checked from chat.
