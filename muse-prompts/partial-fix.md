You were right about kb: c:partial never took effect. LVGL's own canvas calls marked the whole canvas dirty after every frame and cancelled the partial areas. I fixed that in a new firmware, so perf kb now reports what really goes to the screen. Please reread the c:partial section of luapp.admin action "api_gfx". It now spells out the two patterns that work.

The important bit: c:clear() wipes the whole canvas. So "clear, then draw only the paddle, ball and what changed" loses the bricks and walls from the canvas. That only looked fine before because every frame was being redrawn in full. With partial working, use your cover-based build: draw the scene once, then each frame paint over the old spots and draw the moving parts at the new ones, with no clear.

Please:
1. Paddle Ball: install the cover-based build, then read perf. kb should fall from ~315 to a few KB per frame, and frame_ms with it. Then see whether ui.every(16) holds up.
2. Redo your zoom-2 vs native probe (one small moving circle, with partial). The ~36 ms you measured was the zoom scaling applied to the whole canvas every frame. With real partial frames it should only apply to the small changed areas. If zoom still costs a lot, tell me the numbers: a faster zoom is the next thing I can build.
3. F1: it already covers the car's old spots, so kb should drop now too. If frame_ms falls well below 46, try a faster loop (20 to 33 ms).
4. Send me the before/after table again.
