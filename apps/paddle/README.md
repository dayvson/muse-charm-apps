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
scaling proxy), redrawn from a 20 ms timer. The canvas is in
partial mode: every frame starts with a clear and paints only
the paddle, the ball and whatever changed (a broken brick, the
score line), so a frame sends a fraction of the scene. Physics
runs on elapsed time rather than a fixed step, so the game
speed holds whatever the frame rate is, and the ball still
moves in sub-steps so it cannot tunnel through bricks at high
speed; brick bounces reflect off the side the ball actually
came from. On the speed firmware this build measures about
24 fps with 1.3 ms of Lua and 8 ms of drawing per frame,
against 11 fps, 14 ms and 8 ms for the build it replaced
(a 200x200 canvas at zoom 2 repainted in full every frame:
the zoom compositor alone cost more than the game).

## Icon

The app sets its launcher icon at start-up with app.icon:
three bricks, the gold ball and the paddle, drawn on a 96x96
canvas in the game's colours. The board keeps it, so the
launcher shows the artwork instead of the app's initials.
