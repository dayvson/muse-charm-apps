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
  seconds, then settles. Tap to reset.
- Long-press exits back to the launcher, like every app.

## Sound

The Lua API the gadget offers has no sound call in it, so the alarm
is the blinking red screen. If a sound function joins the API, it is
a one-line add in the finish path.

## Installing

Muse installs it with `luapp.install` (id `timer`, name "Timer",
colour 0x34C759), which compiles, saves and opens it on the gadget.
Reinstalling with the same id updates it.
