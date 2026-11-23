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
pixel rows at a time.

One sizing note: the drawing surface is a native 352x352 canvas at
zoom 1 (242 KB), the size the app was first built at; a zoom 2
detour through a 176x176 canvas is retired. The canvas runs in
partial mode, and the sweep never repaints the scene: each frame
paints only the sliver of each disc row whose lit span changed
since the previous frame, plus any text line whose wording
changed, each under its own small cover, and the disc rows
(position and half-width) are computed once at start-up. A full
render happens on the first frame, when the sweep settles, and
on the minute refresh. Measured mid-sweep, a frame sends about
28 KB and takes about 4 ms, at about 27 fps; an earlier build
that repainted the whole text block every frame sent the full
245 KB canvas per frame at 22 fps, because the block's
full-width cover dirtied the whole screen, and the zoom 2
build before that repainted everything at 5.6 fps with 91 ms
of drawing per frame.

## Icon

The app sets its launcher icon at start-up with app.icon: a
cream crescent drawn on a 96x96 canvas over a night sky, with
two small stars. The board keeps it, so the launcher shows the
crescent instead of the app's initials.
