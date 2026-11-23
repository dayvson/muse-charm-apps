-- Moon Phase: a picture of today's moon, drawn on a canvas.
-- The phase is computed from the date (synodic month from a known
-- new moon), and the lit part of the disc is drawn row by row, so
-- the picture is exact for the day. Opening the app plays an
-- animated transition: the terminator sweeps from new moon across
-- the cycle to today's phase in about two and a half seconds.
-- Tap to replay it.
--
-- The canvas is a native 352x352 at zoom 1 (no scaling proxy).
-- It runs in partial mode, and the sweep does not repaint the
-- scene: each frame paints only the sliver of each disc row
-- whose lit span changed, plus the text block under the disc.
-- A full render happens on the first frame, at the end of the
-- sweep, and on the minute refresh.

local BG = 0x07070C
local LIT = 0xE9E2CE
local DARK = 0x1B1B23
local PALE = 0xD8D5CC
local LAV = 0x8E8CA8
local STAR = 0x55566E
local STAR_BRIGHT = 0x9A9BB0

local SYNODIC = 29.530588853
local NEW_MOON = 947182440 -- 2000-01-06 18:14 UTC, a known new moon
local NAMES = {
  "New Moon", "Waxing Crescent", "First Quarter", "Waxing Gibbous",
  "Full Moon", "Waning Gibbous", "Last Quarter", "Waning Crescent",
}
local MONTHS = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"}
local WDAYS = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"}

local CX, CY, R = 176, 146, 86
local ANIM_LEN = 2.4 -- seconds

-- the disc rows, computed once: dy from the centre and the
-- row's half-width, so no frame ever does this sqrt again
local ROWS = {}
for y = -R, R, 2 do
  ROWS[#ROWS + 1] = {y, math.sqrt(R * R - y * y)}
end

local anim_t0 = nil
local anim_to = 0
local prev_age = nil -- the phase the screen shows mid-sweep

local c -- the canvas, created below

local function today_age()
  return ((time.now() - NEW_MOON) / 86400) % SYNODIC
end

-- the lit span of one disc row at a phase, as a, b; the span
-- is empty (b <= a) when the row is fully dark at that phase
local function lit_span(age, rx)
  local cos_t = math.cos(2 * math.pi * age / SYNODIC)
  if age < SYNODIC / 2 then
    return CX + rx * cos_t, CX + rx
  end
  return CX - rx, CX - rx * cos_t
end

local function draw_texts(age)
  local theta = 2 * math.pi * age / SYNODIC
  local illum = math.floor((1 - math.cos(theta)) / 2 * 100 + 0.5)
  local name = NAMES[math.floor(age / SYNODIC * 8 + 0.5) % 8 + 1]
  local waxing = age < SYNODIC / 2
  local d = time.date()
  c:text(name, 176, 248, PALE, 28, "center")
  c:text(illum .. "% lit, day " .. string.format("%.1f", age),
         176, 282, LAV, 16, "center")
  local nxt, what
  if waxing then
    nxt = math.floor(SYNODIC / 2 - age + 0.5); what = "Full moon"
  else
    nxt = math.floor(SYNODIC - age + 0.5); what = "New moon"
  end
  c:text(what .. " in " .. nxt .. (nxt == 1 and " day" or " days"),
         176, 303, LAV, 14, "center")
  c:text(WDAYS[d.wday] .. ", " .. MONTHS[d.month] .. " " .. d.day .. " " ..
         d.year, 176, 322, 0x55566E, 14, "center")
  if not time.synced() then
    c:text("clock not synced yet", 176, 340, 0x55566E, 14, "center")
  end
  return name, illum
end

local function render(age)
  c:clear(BG)

  -- stars, seeded by the day so the sky is stable all day
  local s = math.floor(time.now() / 86400) * 1103515245 + 12345
  local function rnd()
    s = (s * 1103515245 + 12345) % 2147483648
    return s / 2147483648
  end
  for i = 1, 48 do
    local x, y = rnd() * 352, rnd() * 352
    local dx, dy = x - CX, y - CY
    if dx * dx + dy * dy > (R + 14) * (R + 14) and y < 227 then
      if i % 7 == 0 then
        c:circle(x, y, 2, STAR_BRIGHT, {fill = true})
      else
        c:pixel(x, y, STAR)
      end
    end
  end

  -- the disc, row by row: dark side first, then the lit span
  for _, row in ipairs(ROWS) do
    local y, rx = CY + row[1], row[2]
    c:line(CX - rx, y, CX + rx, y, DARK, 2)
    local a, b = lit_span(age, rx)
    if b > a then c:line(a, y, b, y, LIT, 2) end
  end
  c:arc(CX, CY, R, 0, 360, 0x34343F, 2)

  return draw_texts(age)
end

-- one sweep frame: per disc row, paint dark over the parts of
-- the old lit span the new phase no longer covers, and light
-- over the parts it newly covers; then refresh the text block
-- (plain background under the disc, safe to cover wholesale)
local function diff_frame(old_age, new_age)
  for _, row in ipairs(ROWS) do
    local y, rx = CY + row[1], row[2]
    local a1, b1 = lit_span(old_age, rx)
    local a2, b2 = lit_span(new_age, rx)
    if a1 ~= a2 or b1 ~= b2 then
      if b1 > a1 then
        if a2 > a1 then
          c:line(a1, y, math.min(b1, a2), y, DARK, 2)
        end
        if b2 < b1 then
          c:line(math.max(a1, b2), y, b1, y, DARK, 2)
        end
      end
      if b2 > a2 then
        if a1 > a2 then
          c:line(a2, y, math.min(b2, a1), y, LIT, 2)
        end
        if b1 < b2 then
          c:line(math.max(a2, b1), y, b2, y, LIT, 2)
        end
      end
    end
  end
  c:rect(0, 240, 352, 112, BG)
  draw_texts(new_age)
end

local function play()
  anim_to = today_age()
  anim_t0 = time.now()
  prev_age = nil
end

-- launcher icon: a crescent moon in a night sky
do
  local ic = ui.canvas{w = 96, h = 96, color = 0x14141F}
  ic:circle(48, 48, 27, 0xE9E2CE, {fill = true})
  ic:circle(61, 39, 23, 0x14141F, {fill = true})
  ic:circle(70, 64, 2, 0x9A9BB0, {fill = true})
  ic:circle(24, 26, 2, 0x9A9BB0, {fill = true})
  app.icon(ic)
  ic:delete()
end

ui.background(BG) -- the sky colour fills the whole round screen

-- the drawing surface: one native 352x352 canvas at zoom 1,
-- in partial mode so sweep frames send only what they drew
c = ui.canvas{
  w = 352, h = 352, color = BG,
  on_touch = function(kind, x, y) if kind == "down" then play() end end,
}
c:partial(true)

local function ease(p)
  if p < 0.5 then return 2 * p * p end
  return 1 - ((-2 * p + 2) ^ 2) / 2
end

ui.every(20, function()
  if not anim_t0 then return end
  local p = (time.now() - anim_t0) / ANIM_LEN
  if p >= 1 then
    anim_t0 = nil
    prev_age = nil
    local name, illum = render(anim_to)
    print("moon settled: " .. name .. ", " .. illum .. "% lit")
  else
    local age = anim_to * ease(p)
    if prev_age then
      diff_frame(prev_age, age)
    else
      render(age)
    end
    prev_age = age
  end
end)

ui.every(60000, function()
  if not anim_t0 then render(today_age()) end
end)

play()
print("moon ready, transition playing")
local gfx_used, gfx_avail, gfx_largest = gfx.memory()
print("moon gfx memory: used " .. tostring(gfx_used) ..
      ", available " .. tostring(gfx_avail) ..
      ", largest " .. tostring(gfx_largest))
