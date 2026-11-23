# F1

Formula 1 on the gadget, in the Apps by Muse launcher. Lua
source: `f1.lua`. Installed on the gadget with id `f1` and
shown in the launcher as F1.

The app draws on one full-screen 466x466 canvas (created
first, before anything else allocates), so the countdown
segments sit at the true edge of the round screen. All the
content is drawn in a 352 box centred on the screen, shifted
into place by a small proxy. At start-up the app prints
gfx.memory() (used, available, and the largest single block
that fits) to its logs; while it runs that reads about 434
KB used with well over 1 MB still free. Swipe or tap to
move between the four screens; the four dots at the bottom
show where you are.

The canvas is in partial mode and the frame loop is stingy:
the whole scene repaints only when something static actually
changes (the countdown text, a ring segment, fresh data),
and on every other tick only the car moves: its old spots
are covered, the track under them is repaired, and the car
is drawn at its new position, advanced by elapsed time so
the lap stays 4.5 seconds at any frame rate. The 48 ring
segment positions are computed once at start-up instead of
on every repaint. The loop runs every 33 ms and measures
about 30 fps with roughly 11 KB sent and 1.4 ms of frame
time per frame; on the earlier firmware, where partial
never really took effect, the same build sent the whole
424 KB canvas per frame and the flush alone held it to
20 fps at 46 ms per frame.

## The four screens

- Next race: the Grand Prix name over a big drawn outline of
  the circuit, drawn as a road with a pale centre line. A red
  dot laps the circuit like a car, about one lap every four
  and a half seconds, with a short fading trail behind it and
  a white dot marking the start. Near the edge of the screen,
  a ring of 48 small segments lights up from the top
  clockwise for the share of the last seven days still to go
  before the race: gold, then red inside the last day, and
  fully red while the race is on. Under the circuit, the
  start time in local time and a countdown. Tapping the
  circuit swaps this screen to the race after the next one
  (its own outline, date and countdown under a THE ONE AFTER
  heading); tap it again to swap back.
- Weekend schedule: every session of the coming weekend in
  local time (practice, qualifying, the sprint when there is
  one, and the race), in big type with room to breathe, on a
  timeline down the left: one dot per session, coloured by
  kind (practice lavender, sprint blue, qualifying gold, the
  race red). Sessions already over are dimmed; the next one
  coming up gets a ring around its dot and gold text.
- Last race: the previous Grand Prix as a podium ceremony:
  three steps, the winner on the tall middle one, each
  driver over their step with flag, their three-letter code
  in the medal colour, and the gap (the winning time for
  first, plus the grid place when they gained places), the
  place number on the step front, and the race's fastest
  lap along the bottom. The heading is the Grand Prix name
  shortened to GP, as on the schedule screen (Brazilian GP,
  Las Vegas GP, ...).
- Championship: a gold trophy beside a bigger title, then
  the top 3 drivers in the title race. Each row has a bar in
  the place colour (gold, silver, bronze), the driver's flag
  drawn from their nationality, then their name, team,
  points and wins, with the season and round the standings
  are from.

## Lights out

If the app is open in the two minutes after a race starts
(and has not already played it for that race), the five
start lights take over the screen: they come on one by one,
hold, then go out together under a green GO GO GO flash and
a sound. It plays once per race.

## Data

Race, schedule, standings, results and the race after the
next one come from the Ergast mirror at api.jolpi.ca, a free
API that needs no key. They are fetched in a chain when the
app opens (one request at a time, as the gadget requires)
and cached in the app store, so the app opens instantly and
still works offline with the last saved data. The line under
the countdown on the race screen says whether the data on
screen is live or saved.

The API gives times in UTC; the app converts them with its
own date math and the offset it derives from the gadget's
clock, so every time shown is the gadget's local time
wherever it is.

## Circuit outlines

No API serves track shapes, so the outlines are simplified
tracings stored in the app as point lists in a 0..100 box:
the Las Vegas Strip Circuit, Lusail and Yas Marina, the
three circuits left in the 2026 season. A circuit with no
tracing yet gets a location pin instead. Adding one is just
a new point list under its circuit id in `OUTLINES`.

## Flags

The flags are drawn, not images: stripe recipes (vertical or
horizontal thirds, halves) cover most countries, and a few
are drawn specially: the Union Jack, the Nordic crosses, the
Swiss cross, Japan's disc, the United States, Australia,
Brazil, Canada, Thailand and China. A nationality with no
recipe yet gets a plain grey flag.

## Icon

The app sets its launcher icon at start-up with app.icon:
the F1 mark drawn in red on near-black, a slanted F whose
top bar flows into the 1 beside it, built from triangles on
a 96x96 canvas. The board keeps it, so the launcher shows
the mark instead of the app's initials.
