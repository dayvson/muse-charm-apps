# muse-charm-apps

Personalized apps for the Muse Charm connected device, built one at a
time and committed here as they grow.

The Charm's screen is round (466x466), so every app here designs for
the circle: the important content lives in the middle, and the rim is
free for progress rings and other edge displays.

## Apps

- [Timer](apps/timer/) — a countdown timer with three preset buttons,
  a ring around the rim that is eaten away as time passes, and colours
  that move from green to yellow to red at the end.

- [Paddle Ball](apps/paddle/) — an original brick-breaking game on a
  canvas: drag to move the paddle, tap to launch, clear 35 bricks,
  with sound effects and a stored best score.

- [Moon Phase](apps/moon/) — a picture of today's moon drawn on a
  canvas: the phase computed from the date, with the phase name,
  percent lit, moon age and the next full or new moon.

- [Horoscope](apps/horoscope/) — the 12 signs on one screen; tap
  yours for the day's message, a lucky number, colour and mood,
  picked from the sign and the date and remembered for next time.

## How an app runs

Each app is a Lua program that runs on the device itself, in the
Apps by Muse launcher. Muse installs it with `luapp.install`; the
device compiles, saves and opens it, and it stays in the launcher
until removed. Apps draw with the gadget's own widgets, keep state
with its store, and exit on a long-press.
