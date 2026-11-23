I updated my gadget's firmware again, this time for speed in Lua apps. Please reread luapp.admin action "api_gfx" (it changed), then help me tune Paddle Ball and F1.

What changed:
- ui.every used to quietly round anything under 50 ms up to 50 ms, so every game was capped at 20 fps, even with ui.every(33). The floor is now 16 ms (~60 fps).
- New c:partial(true) on a canvas: after it, a frame sends the screen only the areas drawn since the last c:clear() (plus the previous frame's), instead of the whole canvas. A full-screen frame takes about 33 ms to send, so this is the big one for games where small things move on a plain background. Start each frame with c:clear() in the same colour. A full-canvas gradient or tilemap every frame gains nothing.
- New speed readout: gfx.stats() and gfx.show_stats(true) in the app, and luapp.admin action "perf" from your side (no change to the app needed). It gives the last second's fps (screen frames actually redrawn), frame_ms (render and send), kb (redrawn per frame), app_fps, draw_ms, lua_ms, calls and busy (%).

Please, for Paddle Ball and then F1:
1. Open it, wait a few seconds, and read luapp.admin perf. That's the baseline. Note it already benefits from the timer fix.
2. Change it: c:partial(true) where it fits, ui.every(16 to 20) for the game loop, and no new tables or strings per frame where you can avoid it. For F1, if it scrolls the whole track every frame, partial won't help. Tell me what does, using the numbers (draw_ms vs frame_ms vs lua_ms).
3. Reinstall, read perf again, and give me a small before/after table (fps, frame_ms, kb, lua_ms, draw_ms) for each app.
