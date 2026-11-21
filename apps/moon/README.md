# Moon Phase

A picture of today's moon, drawn by the gadget itself in the Apps by
Muse launcher. Lua source: `moon.lua`. Installed on the gadget with
id `moon`.

## What it shows

- The moon disc with the lit part drawn for today's phase, over a
  star field that is stable through the day.
- The phase name (Waxing Gibbous, First Quarter, and so on), the
  percent lit, and the moon's age in days.
- The next milestone: full moon or new moon, in how many days.
- The date. It redraws every minute, so it rolls over at midnight.
- Opening the app plays an animated transition: over about 2.4
  seconds the terminator sweeps from new moon across the cycle to
  today's phase, with the name and percent counting along, then
  settles on the exact picture. Tap to replay it.

## How it works

The phase is computed, not fetched: the moon's age is the time since
a known new moon (2000-01-06 18:14 UTC) modulo the synodic month
(29.530588853 days). The lit span of each row of the disc comes from
the phase angle, the standard terminator-ellipse geometry, drawn two
pixel rows at a time on a 400x400 canvas.

One sizing note: a full-screen 466x466 canvas does not fit the
gadget's graphics memory (the install is refused), so the canvas is
400x400, the same size the Paddle Ball game uses.

## Icon

The app sets its launcher icon at start-up with app.icon: a
cream crescent drawn on a 96x96 canvas over a night sky, with
two small stars. The board keeps it, so the launcher shows the
crescent instead of the app's initials.
