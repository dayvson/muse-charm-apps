-- Icon for Paddle Ball: 16x16 pixel art for gfx.sprite.
-- The launcher tile is drawn by the gadget itself (the app colour
-- plus the name's letters) and the install API has no icon field,
-- so this artwork is kept ready: it drops in as the launcher icon
-- the day the API offers one, and can be shown in-app via ui.image.

local ICON = gfx.sprite{
  rows = {
    "................",
    "................",
    "................",
    "...........oo...",
    "..........oooo..",
    "..........oooo..",
    "...........oo...",
    "................",
    "................",
    "...#########....",
    "...#########....",
    "...#########....",
    "...#########....",
    "...#########....",
    "......###.......",
    "......###.......",
  },
  palette = {["#"] = 0xD8D5CC, ["o"] = 0xFFD60A},
  scale = 4,
}
