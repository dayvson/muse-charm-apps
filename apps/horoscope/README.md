# Horoscope

The 12 signs and a message for the day, running on the gadget in
the Apps by Muse launcher. Lua source: `horoscope.lua`. Installed
on the gadget with id `horoscope`.

## How it works

- One sign per screen. Swipe left or right to move through the
  zodiac (tapping the screen edges works too), with a short slide
  as the new sign comes in. Twelve dots at the bottom show where
  you are, and chevrons mark the edges.
- Each screen is a modern two-column layout on a canvas: text on
  the left, left-aligned (date, sign name, its dates, the day's
  message, the lucky line), and on the right a round medallion, a
  gold-ringed disc holding the sign's glyph, drawn from strokes
  (lines, arcs, circles) in a consistent hand.
- The sign you land on is kept in the app store and opens first
  next time; with no pick yet, it opens on today's sign.

## The daily message

Written for this app: twelve openers, twelve middles and twelve
closers, combined by a hash of the sign and the date. Every sign
gets its own message, it stays the same all day, and it changes
overnight (1,728 combinations). No network, no feed, just the date
and the sign. A lucky number, colour and mood ride along.
