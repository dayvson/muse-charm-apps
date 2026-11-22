# Horoscope

The 12 signs and a message for the day, running on the gadget in
the Apps by Muse launcher. Lua source: `horoscope.lua`. Installed
on the gadget with id `horoscope`.

## The design

Built as a circle for the round screen; nothing lives in the
corners, where the round bezel would clip it.

- A zodiac wheel rings the edge: a thin track with a dot per sign.
  At the top of the wheel sits the app's mark, the gold ring and
  star from the launcher icon. Tap a side of the screen (or swipe)
  and the wheel turns through that mark until the new sign
  settles inside it.
- High in the circle, a round medallion with a soft halo holds
  the sign's glyph, drawn from strokes (lines, arcs, circles) in
  a consistent hand. Below it, all inside the safe disc: the sign
  name, its dates with today's date, the day's message, and the
  lucky line, placed high enough that the circle is still wide
  enough to hold it.
- A faint star band sits outside the wheel. The sign you land on
  is kept in the app store and opens first next time; with no pick
  yet, it opens on today's sign.
- The drawing surface is one full-screen 466x466 canvas (zoom 1).

## The daily message

Written for this app: twelve openers, twelve middles and twelve
closers, combined by a hash of the sign and the date. Every sign
gets its own message, it stays the same all day, and it changes
overnight (1,728 combinations). No network, no feed, just the date
and the sign. A lucky number, colour and mood ride along.

## Icon

The app sets its launcher icon at start-up with app.icon: a
gold ring and a four-point star drawn on a 96x96 canvas over
the app's violet. The board keeps it, so the launcher shows
the star instead of the app's initials.
