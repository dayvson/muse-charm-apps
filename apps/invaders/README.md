# Invaders

The classic fixed shooter, drawn for the round screen.
A formation of twenty invaders in four rows marches
across the sky in steps, reversing and dropping at each
edge, and shoots back. You slide the cannon along the
bottom and shoot them down before they land. Lua
source: `invaders.lua`. Installed on the gadget with
id `invaders` and shown in the launcher as Invaders.

Controls are one finger: drag to steer (the cannon runs
to your finger), tap or hold to fire, at most two shots
in the air at once. The rows score 30, 20, 10 and 10.
Three lives; lose one when a bomb lands, and the game
ends at once if the formation touches down. Clearing a
wave starts the next one lower and faster, and the
formation's march speeds up as it thins out, like the
original. The best score is kept in the app store.

Rendering follows the house pattern for games: one
400x400 canvas at zoom 1 in partial mode. The scene is
painted once, then each frame only covers the movers'
old spots and draws them at the new ones: the invaders
on their march steps, the cannon, the shots and the
bombs, and just the HUD strip that changed. The
invaders are drawn in code as little rect sprites with
two leg frames that swap on every step, and the same
sprite at double size is the launcher icon.
