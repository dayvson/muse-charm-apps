# Horoscope

The 12 signs and a message for the day, running on the gadget in
the Apps by Muse launcher. Lua source: `horoscope.lua`. Installed
on the gadget with id `horoscope`.

## The design

Built as a circle for the round screen; nothing lives in the
corners, where the round bezel would clip it.

- A zodiac wheel rings the edge: a thin track with a dot per sign.
  Swipe (or tap the screen edges) and the wheel turns until the
  new sign reaches the top, marked by a larger gold dot.
- In the middle, a round medallion with a soft halo holds the
  sign's glyph, drawn from strokes (lines, arcs, circles) in a
  consistent hand. Below it, all inside the safe disc: the sign
  name, its dates with today's date, the day's message, and the
  lucky line, which is placed from the message's line count so it
  is always in view.
- A faint star band sits outside the wheel. The sign you land on
  is kept in the app store and opens first next time; with no pick
  yet, it opens on today's sign.

## The daily message

Written for this app: twelve openers, twelve middles and twelve
closers, combined by a hash of the sign and the date. Every sign
gets its own message, it stays the same all day, and it changes
overnight (1,728 combinations). No network, no feed, just the date
and the sign. A lucky number, colour and mood ride along.
