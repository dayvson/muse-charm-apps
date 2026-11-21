# Grand Prix

The next Formula 1 race and the championship top 3, running on
the gadget in the Apps by Muse launcher. Lua source: `f1.lua`.
Installed on the gadget with id `f1`.

## The two screens

Both screens carry a checkered band across the top.

- Next race: the Grand Prix name over a big drawn outline of
  the circuit, drawn as a road with a pale centre line. A red
  dot laps the circuit like a car, about one lap every four and
  a half seconds, with a short fading trail behind it and a
  white dot marking the start. Under it, the start time
  converted to the gadget's local time and a countdown.
- Championship: the top 3 drivers in the title race. Each row
  has a bar in the team's colour, the driver's flag drawn from
  their nationality, then their name, team, points and wins,
  with the season and round the standings are from.

Swipe either way or tap to switch screens; the two dots at the
bottom show where you are.

## Flags

The flags are drawn, not images: stripe recipes (vertical or
horizontal thirds, halves) cover most countries, and a few are
drawn specially: the Union Jack, the Nordic crosses, the Swiss
cross, Japan's disc, the United States, Australia, Brazil,
Canada, Thailand and China. A nationality with no recipe yet
gets a plain grey flag.

## Data

Race and standings come from the Ergast mirror at api.jolpi.ca,
a free API that needs no key. Both are fetched when the app
opens (standings right after the race, since the gadget runs
one request at a time) and cached in the app store, so the app
opens instantly and still works offline with the last saved
data. The line under the countdown says whether the data on
screen is live or saved.

The API gives the race start in UTC; the app converts it with
its own date math and the offset it derives from the gadget's
clock, so the time shown is the gadget's local time wherever
it is.

## Circuit outlines

No API serves track shapes, so the outlines are simplified
tracings stored in the app as point lists in a 0..100 box:
the Las Vegas Strip Circuit, Lusail and Yas Marina, the three
circuits left in the 2026 season. A circuit with no tracing
yet gets a location pin instead. Adding one is just a new
point list under its circuit id in `OUTLINES`.

## Icon

The app sets its launcher icon at start-up with app.icon: a
white circuit loop drawn on a 96x96 canvas over F1 red. The
board keeps it, so the launcher shows the loop instead of the
app's initials.
