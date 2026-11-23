# More widgets, animation and input

Same rules as the widgets in `api`: x, y are the centre from the screen's
centre (inside a scroll, from the scroll's centre), colours 0xRRGGBB, each
returns a widget with set/hide/show/delete. Callbacks get the new value.

    ui.roller{options={"5 min","10 min","25 min"}, value=1, rows=3, x, y, w,
              size=20, bg=0x1C1C1E, color=0x27D3A2, text_color=0x8E8E93,
              on_change=function(i) end}
      -- a spinning picker; value and i are 1-based
    ui.slider{x, y, w=240, h=10, min=0, max=100, value=0, color=0x27D3A2,
              on_change=function(v) end}
    ui.switch{x, y, w=64, h=36, value=0, color=0x27D3A2, bg=0x3A3A3C,
              on_change=function(on) end}          -- on: true/false
    ui.list{items={"Paris","Tokyo","Lima"}, x, y, w=300, h=260, size=20,
            color=0x1C1C1E, text_color=0xFFFFFF, bg, on_select=function(i) end}
      -- a scrolling list of rows (color: each row's, bg: behind them)
    ui.scroll{x, y, w=360, h=300, color, radius}   -- a scrolling area:
      -- make widgets with parent=<the scroll> to put them in it; whatever
      -- reaches past its edges scrolls. No lines, canvases or scrolls inside one.

options and items: up to 100 strings, 64 characters each. `w:set{options=}`
or `w:set{items=}` replaces them; `w:set{value=}` moves a roller, slider or
switch (without calling on_change).

Rollers and sliders take the drag, so swiping over them doesn't change screens.
Leave room around them to swipe away.

## Animation

    ui.animate(w, {x=, y=, opacity=, w=, h=, value=, angle=, scale=}, ms=300,
               {ease="inout", delay=0, repeat=0, reverse=false, on_done=fn})

Moves the widget smoothly from where it is to the given values (any of them).
The screen does the frames, not your timers, so it stays smooth and costs the
app nothing. value: arc, bar, slider. angle (0.1 degrees) and scale (256 = 1x):
images. ease: "linear", "in", "out", "inout", "overshoot", "bounce", "step".
repeat: a count, or true for forever; reverse plays it back each time.
on_done runs once when it ends. `w:stop()` stops its animations where they are.
Prefer ui.animate to moving things in ui.every for slides, fades and pulses.

## Gestures and the button

    ui.on_swipe(function(dir) end)   -- dir "up" or "down" (sideways changes screens)
    ui.on_button(function() end)     -- the bottom button, pressed briefly

While an app has ui.on_button and is on screen, a short press of the bottom
button goes to it instead of putting the gadget to sleep. A long hold still
powers off, and a press while the screen sleeps still wakes it.
`ui.on_button(nil)` gives the button back.
