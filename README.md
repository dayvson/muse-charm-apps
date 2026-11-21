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

## How an app runs

Each app is a Lua program that runs on the device itself, in the
Apps by Muse launcher. Muse installs it with `luapp.install`; the
device compiles, saves and opens it, and it stays in the launcher
until removed. Apps draw with the gadget's own widgets, keep state
with its store, and exit on a long-press.
