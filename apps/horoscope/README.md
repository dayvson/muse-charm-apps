# Horoscope

The 12 signs and a message for the day, running on the gadget in
the Apps by Muse launcher. Lua source: `horoscope.lua`. Installed
on the gadget with id `horoscope`.

## How it works

- One screen shows all 12 signs as a grid of buttons. Tap yours.
- The day screen shows the sign and its dates, today's message, a
  lucky number, a lucky colour and a mood. ALL SIGNS goes back.
- Your sign is kept in the app store, so next time the app opens
  straight to your message.
- A soft select sound plays when you pick a sign.

## The daily message

Written for this app: twelve openers, twelve middles and twelve
closers, combined by a hash of the sign and the date. Every sign
gets its own message, it stays the same all day, and it changes
overnight (1,728 combinations). No network, no feed, just the date
and the sign.

## Icon

`icon.lua` holds a 16x16 zodiac wheel (ring, spokes and hub) in
light violet, drawn as a gfx.sprite. The launcher tile itself is
drawn by the gadget (app colour plus the name's letters); the
install API has no icon field yet, so the artwork is ready for
the day it does.
