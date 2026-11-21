# Timer

A countdown timer that runs on the gadget itself, in the Apps by
Muse launcher. Lua source: `timer.lua`. Installed on the gadget with
id `timer`.

## What it does

- One timer in the middle, starting at 5 minutes, with a round
  minus button on the left and a plus button on the right. Each
  tap changes the duration by a minute (1 to 60 minutes); the
  setting is remembered (store) and restored on the next open.
- Tap anywhere else to start; tap again to pause; tap when it says
  PAUSED to resume.
- A ring around the rim of the round screen shows the time left and
  is eaten away as the timer runs, updated ten times a second by the
  gadget itself.
- The ring is green, turns yellow inside the last minute (or the last
  35% of a short timer), and red for the last stretch.
- At zero the whole screen blinks red with TIME UP and the alarm
  tune loops until a tap stops it and resets the timer.
- Long-press exits back to the launcher, like every app.

## Sound

A soft tick (`sound.effect`, volume 14) sounds each second while
the timer runs. The alarm uses `sound.play` from the gadget's
sound API: an original five second tune (eight quarter notes at
100 bpm, volume 55) that starts again every five seconds, looping
until the timer is stopped with a tap.

## Installing

Muse installs it with `luapp.install` (id `timer`, name "Timer",
colour 0x34C759), which compiles, saves and opens it on the gadget.
Reinstalling with the same id updates it.

## Icon

The app sets its launcher icon at start-up with app.icon: a
clock face drawn on a 96x96 canvas, cream ring and hands on the
app's green. The board keeps it, so the launcher shows the clock
instead of the app's initials.
