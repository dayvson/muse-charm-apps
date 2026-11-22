-- Horoscope: built as a circle for the round screen. A zodiac
-- wheel rings the edge with a dot per sign; swipe and the wheel
-- turns until your sign reaches the top. In the middle, a round
-- medallion holds the sign's glyph, with its name, dates, the
-- day's message and the lucky line stacked inside the safe disc.
-- The message is picked from the sign and the date, so it is the
-- same all day and changes overnight.

local BG = 0x0B0B12
local PALE = 0xD8D5CC
local LAV = 0x8E8CA8
local GOLD = 0xD9A441
local DISC = 0x2A2140
local GLYPH = 0xE9E2CE
local DIM = 0x3A3A48
local FAINT = 0x55566E

local SIGNS = {
  {"Aries", "Mar 21 - Apr 19"}, {"Taurus", "Apr 20 - May 20"},
  {"Gemini", "May 21 - Jun 20"}, {"Cancer", "Jun 21 - Jul 22"},
  {"Leo", "Jul 23 - Aug 22"}, {"Virgo", "Aug 23 - Sep 22"},
  {"Libra", "Sep 23 - Oct 22"}, {"Scorpio", "Oct 23 - Nov 21"},
  {"Sagittarius", "Nov 22 - Dec 21"}, {"Capricorn", "Dec 22 - Jan 19"},
  {"Aquarius", "Jan 20 - Feb 18"}, {"Pisces", "Feb 19 - Mar 20"},
}
local STARTS = {{3,21},{4,20},{5,21},{6,21},{7,23},{8,23},
                {9,23},{10,23},{11,22},{12,22},{1,20},{2,19}}

local OPENERS = {
  "The morning starts slow, but it is setting up something good.",
  "A small conversation today carries more weight than it seems.",
  "Today rewards the thing you have been putting off.",
  "Someone is paying closer attention to you than you think.",
  "The plan will change once today, and the change is the upgrade.",
  "Your energy returns in the afternoon; save the hard thing for then.",
  "A familiar place gives you an unfamiliar idea.",
  "Today is better for finishing than for starting.",
  "News you have been waiting on moves a step closer.",
  "A shortcut tempts you; the long way pays better this time.",
  "You are funnier than usual today. Use it.",
  "An old contact resurfaces with something worth hearing.",
}

local MIDDLES = {
  "Keep money decisions simple until tomorrow.",
  "Say the honest thing early; it lands well.",
  "A walk clears the question you keep circling.",
  "Put the phone down for one hour and the day improves.",
  "Help arrives from an unexpected direction; accept it gracefully.",
  "Your work speaks for itself today, so let it.",
  "Leave room in the schedule for one surprise.",
  "Eat the good meal. You have earned it.",
  "Double-check one detail before you send anything important.",
  "A friend needs the short version of you, not the busy one.",
  "The water, the wind or the road will do you good.",
  "Tidy one small corner of your world and your head follows.",
}

local CLOSERS = {
  "By tonight, you will know more than you do now.",
  "End the day with one page, one song or one call.",
  "Luck is quiet today, but it is on your side.",
  "Tomorrow asks less of you. Today asks focus.",
  "Trust the first answer you gave yourself.",
  "A good day to be early, not late.",
  "Keep one promise you made to yourself.",
  "The stars suggest an early night.",
  "Something small you do today echoes next week.",
  "Be generous with credit; it comes back around.",
  "Finish one thing completely before dinner.",
  "Your sign is showing its best side today.",
}

local COLORS = {"teal", "coral", "gold", "sage", "slate", "cream",
                "rust", "sky", "plum", "olive", "copper", "pearl"}
local MOODS = {"bold", "calm", "curious", "playful", "focused",
               "generous", "patient", "bright", "grounded", "daring",
               "gentle", "sharp"}
local MONTHS = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"}
local WDAYS = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"}

local function pick(pool, sign, salt)
  local day = math.floor(time.now() / 86400) % 10000
  local h = day * 131 + sign * 17 + salt * 101
  return pool[h % #pool + 1]
end

local function lucky_number(sign)
  local day = math.floor(time.now() / 86400) % 10000
  return (day * 7 + sign * 13) % 99 + 1
end

local function wrap(text, width)
  local lines, line = {}, ""
  for word in text:gmatch("%S+") do
    if #line == 0 then
      line = word
    elseif #line + 1 + #word <= width then
      line = line .. " " .. word
    else
      lines[#lines + 1] = line
      line = word
    end
  end
  if #line > 0 then lines[#lines + 1] = line end
  return lines
end

local function sign_for_today()
  local d = time.date()
  local order = {10, 11, 12, 1, 2, 3, 4, 5, 6, 7, 8, 9}
  local cur = 10
  for _, i in ipairs(order) do
    local s = STARTS[i]
    if d.month > s[1] or (d.month == s[1] and d.day >= s[2]) then
      cur = i
    end
  end
  return cur
end

local idx = store.get("sign") or sign_for_today()
local rot = -(idx - 1) * 30 -- wheel rotation, degrees; sign idx on top
local rot_target = rot
local last_day = math.floor(time.now() / 86400)

local c
local GS, GW = 0.72, 4 -- glyph scale and stroke width

local function L(mx, my, x1, y1, x2, y2)
  c:line(mx + x1 * GS, my + y1 * GS, mx + x2 * GS, my + y2 * GS, GLYPH, GW)
end
local function A(mx, my, cx, cy, r, start, sweep)
  c:arc(mx + cx * GS, my + cy * GS, r * GS, start, sweep, GLYPH, GW)
end
local function CIR(mx, my, cx, cy, r, fill)
  c:circle(mx + cx * GS, my + cy * GS, r * GS, GLYPH,
           {fill = fill, width = GW})
end
local function TRI(mx, my, x1, y1, x2, y2, x3, y3)
  c:triangle(mx + x1 * GS, my + y1 * GS, mx + x2 * GS, my + y2 * GS,
             mx + x3 * GS, my + y3 * GS, GLYPH)
end

local GLYPHS = {
  function(mx, my) -- Aries
    A(mx, my, -12, -10, 10, 270, 180) A(mx, my, 12, -10, 10, 270, 180)
    L(mx, my, 0, -10, 0, 20)
  end,
  function(mx, my) -- Taurus
    CIR(mx, my, 0, 9, 13, false) A(mx, my, 0, -4, 18, 195, 150)
  end,
  function(mx, my) -- Gemini
    L(mx, my, -14, -20, -14, 20) L(mx, my, 14, -20, 14, 20)
    L(mx, my, -19, -20, 19, -20) L(mx, my, -19, 20, 19, 20)
  end,
  function(mx, my) -- Cancer
    CIR(mx, my, -11, -9, 6, true) CIR(mx, my, 11, 9, 6, true)
    A(mx, my, -11, -9, 15, 290, 210) A(mx, my, 11, 9, 15, 110, 210)
  end,
  function(mx, my) -- Leo
    CIR(mx, my, -9, -7, 11, false) A(mx, my, 8, 9, 14, 140, 210)
  end,
  function(mx, my) -- Virgo
    L(mx, my, -19, -20, -19, 12) L(mx, my, -7, -20, -7, 12)
    L(mx, my, 5, -20, 5, 2) L(mx, my, 5, 2, 17, 14)
    CIR(mx, my, 15, 16, 5, false)
  end,
  function(mx, my) -- Libra
    A(mx, my, 0, -4, 14, 270, 180)
    L(mx, my, -21, 6, 21, 6) L(mx, my, -21, 19, 21, 19)
  end,
  function(mx, my) -- Scorpio
    L(mx, my, -19, -20, -19, 8) L(mx, my, -7, -20, -7, 8)
    L(mx, my, 5, -20, 5, 8) L(mx, my, 5, 8, 19, 20)
    TRI(mx, my, 19, 20, 8, 18, 16, 9)
  end,
  function(mx, my) -- Sagittarius
    L(mx, my, -15, 15, 13, -13) L(mx, my, -14, 2, -2, 14)
    TRI(mx, my, 20, -20, 3, -15, 15, -3)
  end,
  function(mx, my) -- Capricorn
    L(mx, my, -18, -18, -7, 4) L(mx, my, -7, 4, 5, -18)
    L(mx, my, 5, -18, 5, 0) A(mx, my, 12, 9, 9, 180, 290)
  end,
  function(mx, my) -- Aquarius
    L(mx, my, -22, -10, -11, -18) L(mx, my, -11, -18, 0, -10)
    L(mx, my, 0, -10, 11, -18) L(mx, my, 11, -18, 22, -10)
    L(mx, my, -22, 8, -11, 0) L(mx, my, -11, 0, 0, 8)
    L(mx, my, 0, 8, 11, 0) L(mx, my, 11, 0, 22, 8)
  end,
  function(mx, my) -- Pisces
    A(mx, my, -5, 0, 15, 160, 220) A(mx, my, 5, 0, 15, 340, 220)
    L(mx, my, -17, 0, 17, 0)
  end,
}

local function draw()
  local name, dates = SIGNS[idx][1], SIGNS[idx][2]
  local d = time.date()
  local msg = pick(OPENERS, idx, 1) .. " " .. pick(MIDDLES, idx, 2) ..
              " " .. pick(CLOSERS, idx, 3)
  local lines = wrap(msg, 30)
  if #lines > 6 then lines = wrap(msg, 34) end

  c:clear(BG)

  -- stars in the band outside the wheel, seeded by the day
  local s = math.floor(time.now() / 86400) * 1103515245 + 12345
  local function rnd()
    s = (s * 1103515245 + 12345) % 2147483648
    return s / 2147483648
  end
  for i = 1, 40 do
    local x, y = rnd() * 400, rnd() * 400
    local dx, dy = x - 200, y - 200
    local dd = dx * dx + dy * dy
    if dd > 190 * 190 and dd < 226 * 226 then
      c:pixel(x, y, FAINT)
    end
  end

  -- the zodiac wheel: a track ring and a dot per sign
  c:arc(200, 200, 178, 0, 360, DIM, 2)
  for i = 1, 12 do
    local a = math.rad((i - 1) * 30 + rot)
    local x = 200 + 178 * math.sin(a)
    local y = 200 - 178 * math.cos(a)
    if i == idx then
      c:circle(x, y, 8, GOLD, {fill = true})
      c:circle(x, y, 12, GOLD, {fill = false, width = 2})
    else
      c:circle(x, y, 3, FAINT, {fill = true})
    end
  end

  -- the medallion in the middle
  c:circle(200, 112, 54, DISC, {fill = true, opacity = 110})
  c:circle(200, 112, 44, DISC, {fill = true})
  c:circle(200, 112, 44, GOLD, {fill = false, width = 3})
  GLYPHS[idx](200, 112)

  c:text(name, 200, 168, PALE, 28, "center")
  c:text(dates .. "  -  " .. WDAYS[d.wday] .. ", " .. MONTHS[d.month] ..
         " " .. d.day, 200, 206, GOLD, 14, "center")
  for i, line in ipairs(lines) do
    c:text(line, 200, 238 + (i - 1) * 23, PALE, 16, "center")
  end
  -- fixed spot below the wheel, clear of its bottom dot and track
  c:text("Lucky " .. lucky_number(idx) .. "  -  " .. pick(COLORS, idx, 4) ..
         "  -  " .. pick(MOODS, idx, 5), 200, 386, LAV, 14, "center")
end

local function change(dir)
  idx = (idx - 1 + dir) % 12 + 1
  store.set("sign", idx)
  local target = -(idx - 1) * 30
  while target - rot > 180 do target = target - 360 end
  while target - rot < -180 do target = target + 360 end
  rot_target = target
  sound.effect("select", {volume = 30})
  draw()
end

-- launcher icon: a gold ring and a star on the app's violet
do
  local ic = ui.canvas{w = 96, h = 96, color = 0x6C5CA8}
  ic:circle(48, 48, 31, 0xD9A441, {fill = false, width = 5})
  ic:triangle(48, 25, 55, 48, 41, 48, 0xE9E2CE)
  ic:triangle(48, 71, 55, 48, 41, 48, 0xE9E2CE)
  ic:triangle(25, 48, 48, 55, 48, 41, 0xE9E2CE)
  ic:triangle(71, 48, 48, 55, 48, 41, 0xE9E2CE)
  ic:circle(48, 48, 3, 0xD9A441, {fill = true})
  app.icon(ic)
  ic:delete()
end

ui.background(BG) -- the sky colour fills the whole round screen

local down_x = nil
local function handle_touch(kind, x, y)
  if kind == "down" then
    down_x = x
  elseif kind == "up" and down_x then
    local dx = x - down_x
    if dx < -40 then
      change(1)
    elseif dx > 40 then
      change(-1)
    elseif x < 56 then
      change(-1)
    elseif x > 344 then
      change(1)
    end
    down_x = nil
  end
end

-- the drawing surface is a 200x200 canvas shown at zoom 2,
-- so it fills the screen for a quarter of the memory of a
-- 400 canvas (80 KB instead of 320 KB); this proxy halves
-- every coordinate and size into canvas pixels, and touches
-- are doubled back into the app's 400-wide space
local raw = ui.canvas{
  w = 200, h = 200, zoom = 2, color = BG,
  on_touch = function(kind, x, y)
    handle_touch(kind, x * 2, y * 2)
  end,
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
function c:pixel(x, y, col) raw:pixel(x / 2, y / 2, col) end
function c:line(x1, y1, x2, y2, col, w)
  raw:line(x1 / 2, y1 / 2, x2 / 2, y2 / 2, col, wdt(w)) end
function c:circle(x, y, r, col, o)
  raw:circle(x / 2, y / 2, r / 2, col, opts(o)) end
function c:triangle(x1, y1, x2, y2, x3, y3, col)
  raw:triangle(x1 / 2, y1 / 2, x2 / 2, y2 / 2,
               x3 / 2, y3 / 2, col) end
function c:arc(x, y, r, a0, sweep, col, w)
  raw:arc(x / 2, y / 2, r / 2, a0, sweep, col, wdt(w)) end
function c:text(s, x, y, col, size, align)
  raw:text(s, x / 2, y / 2, col, wdt(size), align) end

ui.every(33, function()
  if rot ~= rot_target then
    rot = rot + (rot_target - rot) * 0.2
    if math.abs(rot_target - rot) < 0.2 then rot = rot_target end
    draw()
  end
end)

ui.every(60000, function()
  local day = math.floor(time.now() / 86400)
  if day ~= last_day then
    last_day = day
    draw()
  end
end)

draw()
print("horoscope ready, on " .. SIGNS[idx][1])
local gfx_used, gfx_avail = gfx.memory()
print("horoscope gfx memory: used " .. tostring(gfx_used) ..
      ", available " .. tostring(gfx_avail))
