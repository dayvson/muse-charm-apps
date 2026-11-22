-- Moon Phase: a picture of today's moon, drawn on a canvas.
-- The phase is computed from the date (synodic month from a known
-- new moon), and the lit part of the disc is drawn row by row, so
-- the picture is exact for the day. Opening the app plays an
-- animated transition: the terminator sweeps from new moon across
-- the cycle to today's phase in about two and a half seconds.
-- Tap to replay it.

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

local anim_t0 = nil
local anim_to = 0

local function today_age()
  return ((time.now() - NEW_MOON) / 86400) % SYNODIC
end

local function render(age)
  local theta = 2 * math.pi * age / SYNODIC
  local illum = math.floor((1 - math.cos(theta)) / 2 * 100 + 0.5)
  local name = NAMES[math.floor(age / SYNODIC * 8 + 0.5) % 8 + 1]
  local cos_t = math.cos(theta)
  local waxing = age < SYNODIC / 2

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
  for y = -R, R, 2 do
    local rx = math.sqrt(R * R - y * y)
    c:line(CX - rx, CY + y, CX + rx, CY + y, DARK, 2)
    local a, b
    if waxing then
      a, b = CX + rx * cos_t, CX + rx
    else
      a, b = CX - rx, CX - rx * cos_t
    end
    if b > a then c:line(a, CY + y, b, CY + y, LIT, 2) end
  end
  c:arc(CX, CY, R, 0, 360, 0x34343F, 2)

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

local function play()
  anim_to = today_age()
  anim_t0 = time.now()
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

-- the drawing surface is a 176x176 canvas shown at zoom 2,
-- so it fills the screen for a quarter of the memory of a
-- 352 canvas (62 KB instead of 242 KB); this proxy halves
-- every coordinate and size into canvas pixels
local raw = ui.canvas{
  w = 176, h = 176, zoom = 2, color = BG,
  on_touch = function(kind, x, y) if kind == "down" then play() end end,
}
local function wdt(w)
  if not w then return w end
  return math.max(1, math.floor(w / 2 + 0.5))
end
local function opts(o)
  if o and o.width then
    local n = {}
    for k, v in pairs(o) do n[k] = v end
    n.width = wdt(o.width)
    return n
  end
  return o
end
c = {}
function c:clear(col) raw:clear(col) end
function c:circle(x, y, r, col, o)
  raw:circle(x / 2, y / 2, r / 2, col, opts(o)) end
function c:pixel(x, y, col) raw:pixel(x / 2, y / 2, col) end
function c:line(x1, y1, x2, y2, col, w)
  raw:line(x1 / 2, y1 / 2, x2 / 2, y2 / 2, col, wdt(w)) end
function c:arc(x, y, r, a0, sweep, col, w)
  raw:arc(x / 2, y / 2, r / 2, a0, sweep, col, wdt(w)) end
function c:text(s, x, y, col, size, align)
  raw:text(s, x / 2, y / 2, col, wdt(size), align) end

local function ease(p)
  if p < 0.5 then return 2 * p * p end
  return 1 - ((-2 * p + 2) ^ 2) / 2
end

ui.every(33, function()
  if not anim_t0 then return end
  local p = (time.now() - anim_t0) / ANIM_LEN
  if p >= 1 then
    anim_t0 = nil
    local name, illum = render(anim_to)
    print("moon settled: " .. name .. ", " .. illum .. "% lit")
  else
    render(anim_to * ease(p))
  end
end)

ui.every(60000, function()
  if not anim_t0 then render(today_age()) end
end)

play()
print("moon ready, transition playing")
local gfx_used, gfx_avail = gfx.memory()
print("moon gfx memory: used " .. tostring(gfx_used) ..
      ", available " .. tostring(gfx_avail))
