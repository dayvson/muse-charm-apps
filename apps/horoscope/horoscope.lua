-- Horoscope: one sign per screen. Swipe left or right (or tap the
-- screen edges) to move through the zodiac; each screen shows the
-- sign's medallion on the right and its message for the day,
-- left-aligned, on the left. The message is picked from the sign
-- and the date, so it is the same all day and changes overnight.
-- The sign you are on is remembered for next time.

local BG = 0x0B0B12
local PALE = 0xD8D5CC
local LAV = 0x8E8CA8
local GOLD = 0xD9A441
local DISC = 0x2A2140
local GLYPH = 0xE9E2CE
local DIM = 0x3A3A48

local SIGNS = {
  {"Aries", "Mar 21 - Apr 19"}, {"Taurus", "Apr 20 - May 20"},
  {"Gemini", "May 21 - Jun 20"}, {"Cancer", "Jun 21 - Jul 22"},
  {"Leo", "Jul 23 - Aug 22"}, {"Virgo", "Aug 23 - Sep 22"},
  {"Libra", "Sep 23 - Oct 22"}, {"Scorpio", "Oct 23 - Nov 21"},
  {"Sagittarius", "Nov 22 - Dec 21"}, {"Capricorn", "Dec 22 - Jan 19"},
  {"Aquarius", "Jan 20 - Feb 18"}, {"Pisces", "Feb 19 - Mar 20"},
}
-- first day of each sign, same order: {month, day}
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
  -- scan the signs in calendar order (Capricorn first, it wraps the
  -- year) and keep the last one whose start date has passed
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
local slide = 0
local last_day = math.floor(time.now() / 86400)

local c -- forward declaration for the glyph helpers

local function L(mx, my, x1, y1, x2, y2)
  c:line(mx + x1, my + y1, mx + x2, my + y2, GLYPH, 5)
end
local function A(mx, my, cx, cy, r, start, sweep)
  c:arc(mx + cx, my + cy, r, start, sweep, GLYPH, 5)
end
local function CIR(mx, my, cx, cy, r, fill)
  c:circle(mx + cx, my + cy, r, GLYPH, {fill = fill, width = 5})
end
local function TRI(mx, my, x1, y1, x2, y2, x3, y3)
  c:triangle(mx + x1, my + y1, mx + x2, my + y2, mx + x3, my + y3, GLYPH)
end

-- one stroke drawing per sign, in a 64x64 box around (mx, my)
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
  local lines = wrap(msg, 33)
  local sx = slide

  c:clear(BG)
  c:text(WDAYS[d.wday] .. ", " .. MONTHS[d.month] .. " " .. d.day,
         44 + sx, 44, LAV, 14, "left")
  c:text(name, 44 + sx, 68, PALE, 28, "left")
  c:text(dates, 44 + sx, 108, GOLD, 14, "left")

  -- the medallion: a round badge for the round screen
  local mx, my = 296 + sx, 96
  c:circle(mx, my, 52, DISC, {fill = true})
  c:circle(mx, my, 52, GOLD, {fill = false, width = 3})
  GLYPHS[idx](mx, my)

  c:line(44, 166, 356, 166, DIM, 2)
  for i, line in ipairs(lines) do
    c:text(line, 44 + sx, 192 + (i - 1) * 25, PALE, 16, "left")
  end
  c:text("Lucky " .. lucky_number(idx) .. " - " .. pick(COLORS, idx, 4) ..
         " - " .. pick(MOODS, idx, 5), 44 + sx, 366, LAV, 14, "left")

  -- chevrons and page dots
  c:line(22, 196, 12, 210, DIM, 3) c:line(12, 210, 22, 224, DIM, 3)
  c:line(378, 196, 388, 210, DIM, 3) c:line(388, 210, 378, 224, DIM, 3)
  for i = 1, 12 do
    local x = 200 + (i - 6.5) * 15
    if i == idx then
      c:circle(x, 390, 4, GOLD, {fill = true})
    else
      c:circle(x, 390, 2, DIM, {fill = true})
    end
  end
end

local function change(dir)
  idx = (idx - 1 + dir) % 12 + 1
  store.set("sign", idx)
  slide = dir * 38
  sound.effect("select", {volume = 30})
  draw()
end

local down_x = nil
c = ui.canvas{
  w = 400, h = 400, color = BG,
  on_touch = function(kind, x, y)
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
  end,
}

ui.every(33, function()
  if slide ~= 0 then
    slide = slide * 0.6
    if math.abs(slide) < 0.6 then slide = 0 end
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
