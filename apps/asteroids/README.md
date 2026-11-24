# Asteroids

The classic vector shooter, drawn the way the original
arcade drew it: everything is contours. The ship is an
outline with a flickering thrust flame, and every
asteroid is an irregular outline polygon, generated per
rock, that drifts and tumbles across the screen. Lua
source: `asteroids.lua`. Installed on the gadget with
id `asteroids` and shown in the launcher as Asteroids.

There are four sizes of asteroid, from a 40 px-radius
huge down to an 11 px small. A destroyed asteroid
splits into two of the next size down, so a huge one
ends as four small ones through its children, and the
two biggest sizes sometimes split into three instead,
for variety. The smallest size just bursts into shards.
Scores run the other way: 20 for a huge, 200 for a
small. Rocks on screen are capped at forty so the
splitting never swamps the board.

Controls are one finger: touch and hold, and the ship
turns toward your finger, thrusts with real inertia
and fires for as long as you hold; release and it
drifts. Everything wraps around the edges. Three
lives, a respawn that waits for the centre to clear
and blinks while it is invulnerable, and one more huge
rock per wave. The best score is kept in the app store.

Rendering follows the house pattern for games: one
400x400 canvas at zoom 1 in partial mode. Everything
drifts every frame, so each frame covers every mover's
old bounding box and redraws its outline at the new
spot: rocks, bullets, the ship, and the little line
shards a rock bursts into. The HUD score, wave and
lives strips repaint only when they change, or when a
rock crosses them.
