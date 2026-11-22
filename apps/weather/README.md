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
fonts are ASCII only), the condition, the day's high and
low, and a translucent glass panel with the next six
hours, each with its own small drawn glyph. Wind and
humidity sit under the panel.

The sky is alive, redrawn 20 times a second on a 233x233
canvas at zoom 2 (106 KB):

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
