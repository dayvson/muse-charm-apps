# Spotify

A Spotify remote for the gadget, in Spotify's own
colours: near-black background, Spotify green, and the
Spotify mark (a green disc with three black arcs) drawn
in code at the top of both screens and as the launcher
icon. Lua source: `spotify.lua`. Installed on the gadget
with id `spotify` and shown in the launcher as Spotify.

Screen 1 is the player: the current track's album art
big in the middle, title and artist under it, a progress
bar flanked by the current time and the total duration,
and Previous / Play-Pause / Next controls (the play
button is the green circle, like the real app). Tapping
the art also toggles play and pause. Screen 2 is
discovery: this week's Release Radar, one new song at a
time with its art, browsed with the side buttons and
played with the green one. Swipe or press the bottom
button to switch screens.

The gadget cannot talk to Spotify itself (that needs the
account login), so the app asks Muse over muse.ask:
"SPOTIFY STATE", "SPOTIFY PLAY", "SPOTIFY PAUSE",
"SPOTIFY NEXT", "SPOTIFY PREV", "SPOTIFY DISCOVER" and
"SPOTIFY PLAYURI spotify:track:<id>". Muse runs the
matching command through the Spotify connector and
answers with a small JSON state, which is also what
luapp.send pushes. The controls only work while Spotify
has a live Connect device (a phone or computer with
Spotify open); when none is live, Muse's answer carries
a note and the app shows "Open Spotify on your phone"
over the art instead of failing silently.

Album art comes from Spotify's public oEmbed page for
the track. Its thumbnail URL carries a size code, and
swapping 00001e02 for 00004851 gives the 64 px version
(about 3 KB, inside the gadget's 32 KB http body cap).
Pictures are cached in the app files as art_<track id>
and redrawn from there next time. One canvas gotcha
cost a debugging round here: the canvas image scale is
a percent, so 300 means 3x; passing 3 drew the art as a
2 px speck.
