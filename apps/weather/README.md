# Weather

A living sky for one city at a time, in the Apps by Muse
launcher. Lua source: `weather.lua`. Installed on the gadget
with id `weather` and shown in the launcher as Weather.

Four cities: Miami (MIA), New York (NYC), Rio de Janeiro
(RIO) and San Francisco (SFO). Tap the city name at the top
to switch to the next city; the content slides up into
place, the new city's saved weather shows instantly, and
fresh data lands a second later. Four dots at the bottom
show which city is on screen, and the last city is
remembered.

The design follows the iPhone Weather app: the whole
screen is a gradient sky painted for the city's condition
and time of day, the city name over a very large
temperature (with a drawn degree ring, since the gadget
fonts are ASCII only), the condition, and a translucent
glass panel with the next three hours, each with its own
small drawn glyph. The day's high and low stack on the
right side; humidity and wind sit side by side on one
line under the temperature.

The sky is alive on one native 466x466 canvas (424 KB),
and the canvas runs in partial mode. (It began on a
233x233 canvas at zoom 2; the zoomed picture looked
coarse and scaled, and the cover rectangles in canvas
pixels rounded a pixel short and left pale trails behind
the clouds, so the app moved to the native canvas, which
is both sharper and faster: no rescale in the flush.)
A full draw paints
the sky, the text and the movers, and happens only when
the scene itself changes: on open, on a city change, on
fresh data, and when a lightning flash ends. Every other
tick (50 ms) is a partial frame: each mover's old spot is
covered, with a few pixels of margin so no antialiased
edge survives, with the background that belongs there (the sky
gradient, or the glass panel's blend inside the panel),
the mover is drawn at its new spot, and any static a
cover or a mover touched (a text line, the sun, a panel
glyph) is repaired. Stars only repaint when they blink on
or off, batched twice a second, and the sun only repaints
when its pulse step changes. Before this, the app
repainted the whole scene every tick and managed 3 to 5
frames a second with 55 to 86 ms of Lua per frame and the
full 424 KB sent every frame; now it holds its 20 fps
tick with about 3 ms of Lua, about 3 ms of drawing,
about 6 ms of frame time, and roughly 37 KB sent per
frame (the movers are spread across the whole sky, so
the flushed region stays wide even though each mover is
small). The temperature digits are drawn at size 142.

- Clear day: blue gradient, a glowing sun that slowly
  pulses, one drifting cloud.
- Clear night: deep blue gradient, a crescent moon and
  42 twinkling stars.
- Partly cloudy: more clouds drift across, sun or moon
  still showing.
- Overcast, fog: grey skies, heavier or softer clouds.
- Rain: dark slate sky and 34 rain streaks falling.
- Snow: pale sky and 30 flakes drifting down with a sway.
- Thunderstorm: a dark violet sky, rain, and a lightning
  flash every few seconds.

Data comes from Open-Meteo (free, no key): current
temperature, condition code and day/night flag, humidity,
wind, the day's high and low, and hourly temperature and
condition. http.get caps URLs at 300 characters, so the
request asks for Celsius and km/h and converts on the
gadget. Each city is cached in the store and refreshed
every 10 minutes while the app is open.

The launcher icon is drawn in code at start-up: a gold sun
coming up behind a white cloud on blue.
