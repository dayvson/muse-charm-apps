# Graphics: images, sprites and canvases

Images and canvases get up to 1.5 MB per app, as much as the board can spare.
`used, available, largest = gfx.memory()`: largest is the biggest single image
or canvas that fits right now (each needs one unbroken block), so check it
before a big one. Each costs w*h*2 bytes (w*h*3 with transparency): a
full-screen canvas is 424 KB, 106 KB with zoom=2.
Sprites cost their unscaled size. Images are 466x466 at most.

## Images

    img = gfx.image(data)   -- PNG or baseline JPEG: the bytes (an http.get body)
                            -- or base64 text (a data: URL works too). Large
                            -- JPEGs are scaled down to fit.
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

Call it at start-up; the board keeps it (unchanged icons aren't rewritten).
It becomes a 96 px circle: opaque images fill it, cropped to the circle;
ones with transparency sit inside at 72 px on the app's `color`, pixel art
enlarged by whole steps (a 12, 18, 24 or 36 px sprite fills it best). For a
crisp, premium look draw a 96x96 canvas: a bold shape or two in 2-3 colours,
no small text; then c:delete(). Make each app's icon distinct.

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
      -- on_touch(kind, x, y): kind "down", "move" (while dragging) or "up".
      --   A canvas with on_touch keeps its drags: swiping on it doesn't
      --   change screens, and holding still for 1.5 s leaves the app.

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
    w, h = c:size()

Draw calls queue (up to 1500 per callback) and draw after it. For a game,
redraw each frame from ui.every(33, ...) (about 30 fps): c:clear(), then
draw. Keep a canvas as small as the game needs: smaller draws faster.

## Example: a paddle game

    local c = ui.canvas{w=360, h=360, color=0x101018,
      on_touch=function(kind, x, y) px = x end}
    local pad = gfx.sprite{rows={"..####..", ".######.", "########"},
      palette={["#"]=0x5AC8FA}, scale=6}
    px, bx, by, vx, vy = 180, 180, 60, 3, 4
    ui.every(33, function()
      bx, by = bx + vx, by + vy
      if bx < 8 or bx > 352 then vx = -vx; sound.effect("click") end
      if by < 8 then vy = -vy end
      if by > 296 and by < 310 and math.abs(bx - px) < 48 then
        vy = -math.abs(vy); sound.effect("coin")
      end
      if by > 360 then bx, by, vy = 180, 60, 4; sound.effect("lose") end
      c:clear()
      c:circle(bx, by, 8, 0xFFD60A)
      c:image(pad, px - 24, 306)
    end)
