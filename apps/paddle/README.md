# Paddle Ball

An original brick-breaking game for the round screen, running on the
gadget itself in the Apps by Muse launcher. Lua source:
`paddle.lua`. Installed on the gadget with id `paddle`.

## How to play

- Drag on the screen to move the paddle. It follows your finger.
- Tap to launch the ball. Three balls per game.
- Break all 35 bricks (5 rows of 7) to clear the level. The top rows
  are worth more: 50, 40, 30, 20, 10 points.
- Where the ball lands on the paddle steers it: centre is straight
  up, the edges send it off at an angle.
- The ball speeds up a little with every brick and every level.
- Your best score is kept in the app store across restarts.
- Hold still for 1.5 seconds to leave the game.

## Sound

Effects from the gadget's sound API: a quiet tick on the walls, a
thock on the paddle, a coin blip per brick, an explosion when a ball
is lost, and win/lose stingers at the end of a level and the game.

## How it is built

One native 400x400 canvas (the game's own coordinate space, no
scaling proxy), driven from a 16 ms timer. The canvas is in
partial mode and the drawing is cover-based: the scene is
painted once, then each frame paints over the paddle's and
the ball's old spots, repairs any static a cover touched (a
wall line, a brick, a text line), and draws the two movers at
their new spots; a frame where nothing changed paints
nothing at all. Physics runs on elapsed time rather than a
fixed step, so the game speed holds whatever the frame rate
is, and the ball still moves in sub-steps so it cannot tunnel
through bricks at high speed; brick bounces reflect off the
side the ball actually came from. In play this measures
about 62 fps with 1.8 ms of Lua and 0.8 ms of drawing per
frame and 3 to 9 KB sent per frame, against 24 fps and
315 KB per frame for the clear-and-repaint build it replaced
(on the earlier firmware partial never really took effect,
so every frame re-sent the whole canvas; a clear per frame
would now wipe the bricks and walls off the canvas, which
is why the cover pattern is the one that works).

## Icon

The app sets its launcher icon at start-up with app.icon:
three bricks, the gold ball and the paddle, drawn on a 96x96
canvas in the game's colours. The board keeps it, so the
launcher shows the artwork instead of the app's initials.
