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

local CX, CY, R = 200, 166, 98
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
    local x, y = rnd() * 400, rnd() * 400
    local dx, dy = x - CX, y - CY
    if dx * dx + dy * dy > (R + 14) * (R + 14) and y < 258 then
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
  c:text(name, 200, 282, PALE, 28, "center")
  c:text(illum .. "% lit, day " .. string.format("%.1f", age),
         200, 320, LAV, 16, "center")
  local nxt, what
  if waxing then
    nxt = math.floor(SYNODIC / 2 - age + 0.5); what = "Full moon"
  else
    nxt = math.floor(SYNODIC - age + 0.5); what = "New moon"
  end
  c:text(what .. " in " .. nxt .. (nxt == 1 and " day" or " days"),
         200, 344, LAV, 14, "center")
  c:text(WDAYS[d.wday] .. ", " .. MONTHS[d.month] .. " " .. d.day .. " " ..
         d.year, 200, 366, 0x55566E, 14, "center")
  if not time.synced() then
    c:text("clock not synced yet", 200, 386, 0x55566E, 14, "center")
  end
  return name, illum
end

local function play()
  anim_to = today_age()
  anim_t0 = time.now()
end

ui.background(BG) -- the sky colour fills the whole round screen

c = ui.canvas{
  w = 400, h = 400, color = BG,
  on_touch = function(kind, x, y) if kind == "down" then play() end end,
}

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
