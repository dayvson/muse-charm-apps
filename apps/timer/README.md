# Timer

A countdown timer that runs on the gadget itself, in the Apps by
Muse launcher. Lua source: `timer.lua`. Installed on the gadget with
id `timer`.

## What it does

- Three preset buttons: 2, 3 or 5 minutes. The pick is remembered
  (store) and restored on the next open.
- Tap anywhere else to start; tap again to pause; tap when it says
  PAUSED to resume.
- A ring around the rim of the round screen shows the time left and
  is eaten away as the timer runs, updated ten times a second by the
  gadget itself.
- The ring is green, turns yellow inside the last minute (or the last
  35% of a short timer), and red for the last stretch.
- At zero the whole screen blinks red with TIME UP for about six
  seconds and a short two-tone alarm plays, twice. Then it settles;
  tap to reset.
- Long-press exits back to the launcher, like every app.

## Sound

The alarm uses `sound.play` from the gadget's sound API: an original
two-tone melody (A5/F5 eighths at 170 bpm, volume 55), played at
zero and once more halfway through the blink. Tapping to reset
stops it.

## Installing

Muse installs it with `luapp.install` (id `timer`, name "Timer",
colour 0x34C759), which compiles, saves and opens it on the gadget.
Reinstalling with the same id updates it.

## Icon

The app sets its launcher icon at start-up with app.icon: a
clock face drawn on a 96x96 canvas, cream ring and hands on the
app's green. The board keeps it, so the launcher shows the clock
instead of the app's initials.
