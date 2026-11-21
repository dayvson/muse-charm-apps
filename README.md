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

- [Horoscope](apps/horoscope/) — a zodiac wheel for the round
  screen: swipe and the wheel turns your sign to the top, with a
  medallion and the day's message in the middle.

- [Converter](apps/fx/) — USD, BRL, EUR, GBP and RUB with each
  side switchable, a keypad for the amount, and live rates from
  free keyless sources, cached for offline use.

- [F1](apps/f1/) — Formula 1 inside a checkered ring: the
  next race with a lapping car on a drawn circuit outline, the
  weekend schedule, the last race's podium and the
  championship top 3, from a free keyless API.

## How an app runs

Each app is a Lua program that runs on the device itself, in the
Apps by Muse launcher. Muse installs it with `luapp.install`; the
device compiles, saves and opens it, and it stays in the launcher
until removed. Apps draw with the gadget's own widgets, keep state
with its store, and exit on a long-press.
