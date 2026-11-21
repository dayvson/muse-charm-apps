-- Icon for Timer: 16x16 pixel art for gfx.sprite.
-- The launcher tile is drawn by the gadget itself (the app colour
-- plus the name's letters) and the install API has no icon field,
-- so this artwork is kept ready: it drops in as the launcher icon
-- the day the API offers one, and can be shown in-app via ui.image.

local ICON = gfx.sprite{
  rows = {
    "................",
    ".....######.....",
    "...##########...",
    "..###......###..",
    "..##....#...##..",
    ".##.....#....##.",
    ".##.....#....##.",
    ".##....#####.##.",
    ".##....##....##.",
    ".##..........##.",
    ".##..........##.",
    "..##........##..",
    "..###......###..",
    "...##########...",
    ".....######.....",
    "................",
  },
  palette = {["#"] = 0x34C759},
  scale = 4,
}
