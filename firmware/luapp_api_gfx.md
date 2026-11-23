# Graphics: images, sprites and canvases

Images and canvases get up to 1.5 MB per app, as the board can spare.
`used, available, largest = gfx.memory()`: largest is the biggest single
image or canvas that fits now; check it before a big one. Each costs w*h*2
bytes (w*h*3 with transparency): a full-screen canvas is 424 KB, 106 KB with
zoom=2. Sprites cost their unscaled size. Images are 466x466 at most.

## Images

    img = gfx.image(data)   -- PNG or baseline JPEG: bytes (an http.get body) or
                            -- base64 (or a data: URL); big JPEGs scale down
    img = gfx.sprite{rows={"..##..", ".#..#."}, palette={["#"]=0xFF4040}, scale=4}
                            -- pixel art: one character per pixel; characters
                            -- not in the palette are transparent; scale
                            -- enlarges it when drawn, at no memory cost
    img.w, img.h
    img:free()              -- release it now (unused images are freed anyway)

PNG: any colour type, transparency kept, not interlaced. Embed small images
as base64 in the code; fetch larger ones with http.get (32 KB at most).

    ui.image{img=img, x, y, scale=100, angle=0, opacity=255, on_tap}
      -- centred at x, y like other widgets; scale in percent; angle in
      -- degrees clockwise. w:set{img=other, x=..., angle=...} moves, swaps
      -- or spins it.

## Launcher icon

    app.icon(img)           -- an image, sprite or canvas; app.icon(nil) clears

Call it at start-up; the board keeps it. It becomes a 96 px circle: opaque
images fill it, cropped; transparent ones sit inside at 72 px on the app's
`color` (a 12, 18, 24 or 36 px sprite fills it best). For a crisp look draw
a 96x96 canvas: a bold shape or two in 2-3 colours, no small text; then
c:delete(). Make each app's icon distinct.

## Canvas

A drawing surface for games and custom graphics. Draw in any callback; the
screen shows the result when the callback returns. On a canvas, coordinates
are pixels from its top-left corner (fractions round).

    c = ui.canvas{x=0, y=0, w=200, h=200, color=0x000000, zoom=1, on_tap, on_touch}
      -- x, y: its centre, like other widgets. 2 canvases at most.
      -- zoom=2 to 4: shown that many times bigger with crisp pixels, for a
      --   quarter (or less) of the memory; w=233, h=233, zoom=2 fills the
      --   screen. Draw and touch in canvas pixels.
      -- on_tap(x, y): a tap, in canvas coordinates
      -- on_touch(kind, x, y): "down", "move" or "up". It keeps its drags
      --   (no screen swipes); holding still 1.5 s still leaves the app.

    c:clear([color])                       -- fill (default: the canvas colour)
    c:rect(x, y, w, h, color [, {fill=true, width=2, radius=0, opacity=255}])
    c:circle(cx, cy, r, color [, {fill=true, width=2, opacity=255}])
    c:line(x1, y1, x2, y2, color [, width=2])
    c:triangle(x1, y1, x2, y2, x3, y3, color)
    c:arc(cx, cy, r, start, sweep, color [, width=4])  -- degrees, clockwise from the top
    c:text(text, x, y [, color=0xFFFFFF [, size=16 [, align="left"]]])
      -- y is the top of the text; align "left" | "center" | "right" about x
    c:image(img, x, y [, {scale=100, angle=0, opacity=255}])
      -- top-left at x, y; scale grows right and down; angle turns it about
      -- its centre
    c:pixel(x, y, color)
    c:polygon({x1, y1, x2, y2, ...}, color [, opacity])  -- filled, 3-32 corners
    c:gradient(x, y, w, h, color1, color2 [, "v" | "h"]) -- or {dir, radius, opacity}
    c:tilemap(sheet, tw, th, map, cols [, x=0, y=0])
      -- sheet: tw x th tiles numbered from 1, left to right then down (a
      -- sprite sheet's scale applies); map: tile numbers, cols per row, 0
      -- empty, 4096 at most. Draws the whole map in one call.
    c:sprites(img, {x1, y1, x2, y2, ...})  -- img at up to 256 top-lefts
    w, h = c:size()

Draw calls queue (up to 1500 per callback) and draw after it; tilemap,
sprites and polygon count as one each. Pixels stay between callbacks. A
callback that draws re-sends the whole canvas to the screen (full screen:
~33 ms, so 30 fps at most); one that doesn't costs nothing.

    c:partial(true)   -- send only what changed: for games

After it, a frame sends the screen only where its calls painted. c:clear()
wipes the whole canvas, so the frame after a clear also re-sends where the
previous frame painted. Two ways that work:
- Small scenes: c:clear() then draw everything, every frame.
- A scene that mostly stays: draw it once; then each frame paint over the
  moving things' old spots (background colour, or redraw what was under
  them) and draw them at the new spots. No clear: only those spots are sent.
Never clear and then draw only part of the scene: the rest is gone from the
canvas. A full-canvas gradient or tilemap every frame gains nothing. Run
games from ui.every(16, ...) to ui.every(33, ...).

    gfx.stats()   -- the last second: {fps, frame_ms, kb (per frame), app_fps,
                  --  draw_ms, lua_ms, calls, busy (%), lua_kb}
    gfx.show_stats(true)   -- the same, on screen, while you tune

Keep moving parts small and static ones in widgets; smaller canvases draw
faster.

## Example: a paddle game

    local c = ui.canvas{w=360, h=360, color=0x101018,
      on_touch=function(kind, x, y) px = x end}
    c:partial(true)
    local pad = gfx.sprite{rows={"..####..", ".######.", "########"},
      palette={["#"]=0x5AC8FA}, scale=6}
    px, bx, by, vx, vy = 180, 180, 60, 3, 4
    ui.every(20, function()
      bx, by = bx + vx, by + vy
      if bx < 8 or bx > 352 then vx = -vx; sound.effect("click") end
      if by < 8 then vy = -vy end
      if by > 296 and by < 310 and math.abs(bx - px) < 48 then vy = -vy end
      if by > 360 then bx, by, vy = 180, 60, 4; sound.effect("lose") end
      c:clear()
      c:circle(bx, by, 8, 0xFFD60A)
      c:image(pad, px - 24, 306)
    end)
