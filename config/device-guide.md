<!-- muse-charm: the device guide Muse reads (device.guide). Based on the SDK's devices/guides/muse.md, plus the apps. Max 4096 bytes. -->
# Muse Gadget with a screen

An ESP32 board running the Muse Gadget SDK firmware with its own screen: it
shows Muse's avatar and, on boards with a microphone, people talk to Muse
from it with voice notes whose replies show on the screen. Muse uses it
through the commands it lists.

## What it can do

- On boards with a microphone, people talk to Muse from it: holding the talk
  button (or the on-screen microphone on touch-only boards) records a voice
  note, sent on release. On
  boards set to toggle recording, one press starts the note and the next
  sends it. Replies show on the screen.
- If it lists `display.*` commands, Muse can show images on its screen. An
  image replaces the usual screen until `display.show_animation` or a press
  on the device; bring it back when the user is done.
- If it lists `camera.capture`, it has a camera Muse can take a still photo
  with when the user asks.
- If it lists `sensors.read`, it reports the room's air readings.
- If it lists `device.discover`, it's a bridge to the home network. The scan
  runs from the gadget and takes up to about 90 seconds: work from the last
  result rather than starting another. Muse can reach the devices it finds
  through the gadget's network tunnel.
- If it lists `ble.*` commands, it can scan for and talk to nearby Bluetooth
  LE devices: one at a time, and only devices the user means.
- `device.health` reports battery and charging on boards with a battery.

## Watch (two swipes right of Muse, past the latest reply)

Dual time. **Local** (Meta-blue hour hand, date, LOCAL window) is where the user
is: they live in Miami, so MIA at home; when they travel set it to where they
are, and back to MIA when home. **Work** (copper skeleton hand, WORK window) is
SFO, the zone they follow and their "second time zone"; it stays put unless
asked. Keep local in step when you learn they've moved (they say so, calendar,
flights) and tell them.

## Stocks (two swipes)

META, SHOP and SPY: price, change and chart over 1D to 5Y (Yahoo Finance).

## Wind (three swipes)

Kite forecast in knots for Crandon Park and Stiltsville (Key Biscayne, near
home). For anywhere else the user asks about, look up its coordinates and call
`wind.set_spot`.

## Apps (one swipe left of Muse, before Settings)

Lua apps you build for the user, installed without a firmware update. Read
`luapp.admin` action `api` first (and `api_ui` for more widgets and
animation, `api_gfx` for images, sprites and games, `api_sound` for sound,
`api_device` for muse.ask, sensors, alarms and files), then `luapp.install`; fix any error it
returns, and check `logs` after the user tries it. Give each app its own
launcher icon with `app.icon` (see `api_gfx`). Show apps with `app.show`.

## Controls

- The talk button, or the on-screen microphone on touch-only boards, where
  the board has a microphone.
- The settings menu, with the device's options.

## Recovery

- Settings > Reset pairing forgets Wi-Fi and the pairing; set it up again in
  the Muse app.
- If it's frozen, turn it off and on again.

## Limits

- It has no shell or files Muse can reach: it does only what its commands
  offer.
- Commands work only while it's connected.
