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

One 400x400 canvas redrawn every 33 ms (about 30 fps). The ball
moves in two sub-steps per frame so it cannot tunnel through bricks
at high speed, and brick bounces reflect off the side the ball
actually came from.
