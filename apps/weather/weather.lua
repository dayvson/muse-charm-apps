-- Weather: one city at a time, drawn like a living sky.
-- Tap the city name to switch to the next city (Miami, New
-- York, Rio de Janeiro, San Francisco). The sky is painted
-- for the city's condition and time of day: a gradient sky,
-- a glowing sun or a crescent moon with twinkling stars,
-- drifting clouds, falling rain or snow, and the odd
-- lightning flash in a storm. Under the big temperature, a
-- glass panel shows the next three hours. Data comes from
-- Open-Meteo (free, no key) and is cached per city in the
-- store, so the app opens instantly on saved data.
--
-- The canvas runs in partial mode. A full draw paints the
-- sky, the statics and the movers (on open, on a city
-- change, on new data, after a lightning flash). Between
-- full draws, a frame only moves the movers: it covers
-- each mover's old spot with the background that belongs
-- there (sky gradient, or the glass panel's blend inside
-- the panel), draws it at its new spot, and repairs any
-- static a cover or a mover touched (a text line, the
-- sun, a panel glyph). Nothing else is repainted.

local CITIES = {
  {code = "MIA", name = "Miami", lat = "25.79", lon = "-80.22"},
  {code = "NYC", name = "New York", lat = "40.71", lon = "-74.01"},
  {code = "RIO", name = "Rio de Janeiro", lat = "-22.91",
   lon = "-43.17"},
  {code = "SFO", name = "San Francisco", lat = "37.77",
   lon = "-122.42"},
}

local WHITE = 0xFFFFFF
local SOFT = 0xDCE9F7

-- the drawing surface is one full-screen 466x466 canvas
-- at zoom 1, created first. An earlier build drew on a
-- 233x233 canvas at zoom 2 through a halving proxy, but
-- the scaled pixels looked coarse and the halved cover
-- rects rounded a pixel short, so movers left pale trails
-- and the scene shimmered. All drawing code works in
-- screen space; the thin proxy below only adds yoff,
-- which slides the content (not the sky) during a city
-- change.
local touch_fn = nil
local raw = ui.canvas{
  w = 466, h = 466, color = 0x101D3D,
  on_touch = function(kind, x, y)
    if touch_fn then touch_fn(kind, x, y) end
  end,
}
raw:partial(true)
local yoff = 0
local function px(v) return v end
local function py(v) return v + yoff end
local function half(v) return v end
local function wdt(w) return w end
local function opts(o) return o end
local c = {}
function c:clear(col) raw:clear(col) end
function c:rect(x, y, w, h, col, o)
  raw:rect(px(x), py(y), half(w), half(h), col, opts(o)) end
function c:circle(x, y, r, col, o)
  raw:circle(px(x), py(y), half(r), col, opts(o)) end
function c:line(x1, y1, x2, y2, col, w)
  raw:line(px(x1), py(y1), px(x2), py(y2), col, wdt(w)) end
function c:triangle(x1, y1, x2, y2, x3, y3, col)
  raw:triangle(px(x1), py(y1), px(x2), py(y2),
               px(x3), py(y3), col) end
function c:text(s, x, y, col, size, align)
  raw:text(s, px(x), py(y), col, wdt(size), align) end

-- colour helpers
local function ch(col, shift)
  return math.floor(col / shift) % 256
end
local function lerp_col(a, b, t)
  local r = ch(a, 65536) + (ch(b, 65536) - ch(a, 65536)) * t
  local g = ch(a, 256) + (ch(b, 256) - ch(a, 256)) * t
  local bl = ch(a, 1) + (ch(b, 1) - ch(a, 1)) * t
  return math.floor(r) * 65536 + math.floor(g) * 256
         + math.floor(bl)
end

-- weather code (WMO) to a scene group and a label
local function group_of(code)
  if code == 0 then return "clear" end
  if code == 1 or code == 2 then return "partly" end
  if code == 3 then return "overcast" end
  if code == 45 or code == 48 then return "fog" end
  if code >= 51 and code <= 67 then return "rain" end
  if code >= 80 and code <= 82 then return "rain" end
  if code >= 71 and code <= 77 then return "snow" end
  if code == 85 or code == 86 then return "snow" end
  if code >= 95 then return "thunder" end
  return "partly"
end
local LABELS = {
  [0] = "Clear", [1] = "Mainly Clear", [2] = "Partly Cloudy",
  [3] = "Overcast", [45] = "Fog", [48] = "Fog",
  [51] = "Light Drizzle", [53] = "Drizzle", [55] = "Drizzle",
  [56] = "Freezing Drizzle", [57] = "Freezing Drizzle",
  [61] = "Light Rain", [63] = "Rain", [65] = "Heavy Rain",
  [66] = "Freezing Rain", [67] = "Freezing Rain",
  [71] = "Light Snow", [73] = "Snow", [75] = "Heavy Snow",
  [77] = "Snow", [80] = "Light Showers", [81] = "Showers",
  [82] = "Heavy Showers", [85] = "Snow Showers",
  [86] = "Snow Showers", [95] = "Thunderstorm",
  [96] = "Thunderstorm", [99] = "Thunderstorm",
}

-- sky palettes: {top, mid, bottom} per group and day/night
local SKY = {
  clear    = {day = {0x1F5FD6, 0x4E9BE8, 0xAEDCF7},
              night = {0x050914, 0x101D3D, 0x274069}},
  partly   = {day = {0x2B66C4, 0x5E97D8, 0xBCD9F0},
              night = {0x0A1226, 0x1B2C50, 0x39547E}},
  overcast = {day = {0x4A5D75, 0x74879C, 0xA9BACA},
              night = {0x11161F, 0x232C3A, 0x3E4C5E}},
  rain     = {day = {0x2E4258, 0x506A84, 0x84A0B4},
              night = {0x0A1018, 0x1A2634, 0x2E4254}},
  snow     = {day = {0x6E8DB4, 0xA8C2DC, 0xE4EEF8},
              night = {0x1A2436, 0x2E405C, 0x546A8C}},
  thunder  = {day = {0x171226, 0x2E2748, 0x4C4266},
              night = {0x171226, 0x2E2748, 0x4C4266}},
  fog      = {day = {0x5E6B76, 0x8B99A4, 0xBEC8CF},
              night = {0x141A20, 0x28323C, 0x46525E}},
}

local idx = store.get("city") or 1
if idx < 1 or idx > #CITIES then idx = 1 end
local wx = nil      -- weather for the city on screen
local tick = 0
local slide = 0     -- city-change animation, 1 down to 0
local flash = 0     -- lightning flash frames left
local DEFW = {temp = 0, code = 1, is_day = true, hi = 0,
              lo = 0, hum = 0, wind = 0, hours = {}}
local cur_pal = SKY.partly.day -- palette on screen now
local need_full = true         -- next frame repaints all
local prev_flash_on = false
local prev_pulse = nil         -- last drawn sun pulse step

local function split(s, sep)
  local t, pos = {}, 1
  while true do
    local i = s:find(sep, pos, true)
    if not i then t[#t + 1] = s:sub(pos) break end
    t[#t + 1] = s:sub(pos, i - 1)
    pos = i + 1
  end
  return t
end

local function load_cache(city)
  local s = store.get("wx" .. city.code)
  if not s then return nil end
  local f = split(s, "|")
  if #f < 8 then return nil end
  local w = {temp = tonumber(f[1]), code = tonumber(f[2]),
             is_day = f[3] == "1", hi = tonumber(f[4]),
             lo = tonumber(f[5]), hum = tonumber(f[6]),
             wind = tonumber(f[7]), hours = {}}
  for entry in f[8]:gmatch("[^;]+") do
    local h, t, cd = entry:match("^(%d+),(%-?%d+),(%d+)$")
    if h then
      w.hours[#w.hours + 1] = {hour = tonumber(h),
        temp = tonumber(t), code = tonumber(cd)}
    end
  end
  return w
end

local function save_cache(city, w)
  local hs = ""
  for i, h in ipairs(w.hours) do
    if i > 1 then hs = hs .. ";" end
    hs = hs .. h.hour .. "," .. h.temp .. "," .. h.code
  end
  store.set("wx" .. city.code, table.concat({
    w.temp, w.code, w.is_day and 1 or 0, w.hi, w.lo,
    w.hum, w.wind, hs}, "|"))
end

-- animated sky pieces, seeded once so they are stable
local stars = {}
for i = 1, 42 do
  stars[i] = {x = (i * 173) % 466, y = (i * 97) % 210,
              p = i * 0.7}
end
local clouds = {
  {x = 50, y = 158, s = 0.8, v = 0.35},
  {x = 395, y = 170, s = 0.65, v = 0.35},
  {x = 120, y = 215, s = 0.55, v = 0.35},
  {x = 330, y = 228, s = 0.5, v = 0.35},
}
local rain = {}
for i = 1, 34 do
  rain[i] = {x = (i * 61) % 466, y = (i * 89) % 466,
             l = 16 + (i % 5) * 4, v = 13 + (i % 4) * 3}
end
local snow = {}
for i = 1, 30 do
  snow[i] = {x = (i * 53) % 466, y = (i * 71) % 466,
             r = 2 + (i % 3), v = 1.1 + (i % 4) * 0.4,
             p = i * 1.3}
end

-- the background that belongs at a screen position: the
-- sky gradient colour of that band, or, inside the glass
-- panel, the panel colour blended over the sky there
local function sky_col_at(y)
  local band = math.floor(y / 12) * 12
  local t = band / 466
  if t < 0.55 then
    return lerp_col(cur_pal[1], cur_pal[2], t / 0.55)
  end
  return lerp_col(cur_pal[2], cur_pal[3], (t - 0.55) / 0.45)
end
local function panel_col_at(y)
  return lerp_col(sky_col_at(y), 0x0B1D38, 105 / 255)
end

-- repaint the background over a rect, band by band, so a
-- cover matches the gradient (and the panel) underneath
local function paint_bg(x, y, w, h)
  if w <= 0 or h <= 0 then return end
  local y2 = y + h
  local by = math.floor(y / 12) * 12
  while by < y2 do
    local ty = math.max(by, y)
    local bh = math.min(by + 12, y2) - ty
    if bh > 0 then
      if wx and ty >= 316 and ty < 400 then
        if x < 83 then
          c:rect(x, ty, math.min(x + w, 83) - x, bh,
                 sky_col_at(ty))
        end
        local px1 = math.max(x, 83)
        local px2 = math.min(x + w, 383)
        if px2 > px1 then
          c:rect(px1, ty, px2 - px1, bh, panel_col_at(ty))
        end
        if x + w > 383 then
          local rx = math.max(x, 383)
          c:rect(rx, ty, x + w - rx, bh, sky_col_at(ty))
        end
      else
        c:rect(x, ty, w, bh, sky_col_at(ty))
      end
    end
    by = by + 12
  end
end

-- screen areas painted this frame (covers and movers), so
-- the statics they touched can be repaired afterwards
local touches = {}
local touches_n = 0
local function touch(x1, y1, x2, y2)
  touches_n = touches_n + 1
  local t = touches[touches_n]
  if not t then t = {} touches[touches_n] = t end
  t[1], t[2], t[3], t[4] = x1, y1, x2, y2
end
local function touched(x1, y1, x2, y2)
  for i = 1, touches_n do
    local t = touches[i]
    if x1 < t[3] and t[1] < x2 and y1 < t[4] and t[2] < y2 then
      return true
    end
  end
  return false
end

local function draw_sky(pal)
  cur_pal = pal
  yoff = 0
  local band = 12
  for y = 0, 466 - band, band do
    local t = y / 466
    local col
    if t < 0.55 then col = lerp_col(pal[1], pal[2], t / 0.55)
    else col = lerp_col(pal[2], pal[3], (t - 0.55) / 0.45) end
    c:rect(0, y, 466, band + 1, col)
  end
end

local function star_alpha(s)
  return 120 + 110 * math.sin(tick * 0.06 + s.p)
end
local function draw_star(s)
  local a = star_alpha(s)
  s.on = a > 40
  s.b = math.floor(a / 32)
  if s.on then
    c:circle(s.x, s.y, 2, WHITE, {fill = true, opacity = a})
  end
end

local function pulse_amt()
  return math.floor(4 * math.sin(tick * 0.03) + 0.5)
end
local function draw_sun(x, y)
  local pulse = pulse_amt()
  prev_pulse = pulse
  c:circle(x, y, 58 + pulse, 0xFFD60A, {fill = true, opacity = 40})
  c:circle(x, y, 44 + pulse, 0xFFD60A, {fill = true, opacity = 70})
  c:circle(x, y, 32, 0xFFDE59, {fill = true})
  c:circle(x, y, 32, 0xFFF3B0, {fill = false, width = 3})
end

local function draw_moon(x, y, skycol)
  c:circle(x, y, 30, 0xF4F1DE, {fill = true})
  c:circle(x + 14, y - 8, 26, skycol, {fill = true})
  c:circle(x - 8, y + 10, 3, 0xD9D4BE, {fill = true})
end

local function draw_celestial()
  local w = wx or DEFW
  local g = group_of(w.code)
  if g ~= "clear" and g ~= "partly" then return end
  local pal = SKY[g][w.is_day and "day" or "night"]
  if w.is_day then draw_sun(368, 118)
  else draw_moon(368, 118, pal[2]) end
end

local function cloud_style(g, is_day)
  local col, op = WHITE, is_day and 205 or 110
  if g == "overcast" or g == "thunder" then
    col, op = 0x9AA7B5, 225
  elseif g == "rain" then
    col, op = 0xB9C7D4, 215
  elseif g == "fog" then
    col, op = 0xC7D0D8, 160
  end
  local n = (g == "clear") and 1 or (g == "partly") and 3 or 4
  return col, op, n
end

local function draw_cloud(cl, col, op)
  local s = cl.s
  c:circle(cl.x, cl.y, 24 * s, col, {fill = true, opacity = op})
  c:circle(cl.x + 26 * s, cl.y + 4 * s, 19 * s, col,
           {fill = true, opacity = op})
  c:circle(cl.x - 26 * s, cl.y + 5 * s, 18 * s, col,
           {fill = true, opacity = op})
  c:circle(cl.x + 4 * s, cl.y - 10 * s, 18 * s, col,
           {fill = true, opacity = op})
  c:rect(cl.x - 44 * s, cl.y + 2 * s, 88 * s, 16 * s, col,
         {opacity = op})
  cl.dx = math.floor(cl.x + 0.5)
end

local function draw_rain_drop(r)
  c:line(r.x, r.y, r.x - 3, r.y + r.l, 0xAFD4F5, 2)
  r.px, r.py = r.x, r.y
end

local function snow_pos(s)
  return s.x + 8 * math.sin(tick * 0.05 + s.p), s.y
end
local function draw_snow_flake(s)
  local sx, sy = snow_pos(s)
  c:circle(sx, sy, s.r, WHITE, {fill = true, opacity = 220})
  s.px, s.py = sx, sy
end

-- a small condition glyph for the hourly strip
local function draw_glyph(g, x, y, night)
  if g == "clear" then
    if night then
      c:circle(x, y, 8, 0xF4F1DE, {fill = true})
      c:circle(x + 4, y - 2, 7, 0x10233F, {fill = true})
    else
      c:circle(x, y, 8, 0xFFD60A, {fill = true})
      for k = 0, 7 do
        local a = k * math.pi / 4
        c:line(x + 11 * math.cos(a), y + 11 * math.sin(a),
               x + 14 * math.cos(a), y + 14 * math.sin(a),
               0xFFD60A, 2)
      end
    end
  elseif g == "rain" then
    c:circle(x - 4, y - 3, 7, SOFT, {fill = true})
    c:circle(x + 4, y - 4, 6, SOFT, {fill = true})
    c:line(x - 6, y + 5, x - 8, y + 10, 0x7FC4FF, 2)
    c:line(x, y + 5, x - 2, y + 10, 0x7FC4FF, 2)
    c:line(x + 6, y + 5, x + 4, y + 10, 0x7FC4FF, 2)
  elseif g == "snow" then
    c:circle(x - 4, y - 3, 7, SOFT, {fill = true})
    c:circle(x + 4, y - 4, 6, SOFT, {fill = true})
    c:circle(x - 5, y + 7, 2, WHITE, {fill = true})
    c:circle(x + 1, y + 9, 2, WHITE, {fill = true})
    c:circle(x + 6, y + 7, 2, WHITE, {fill = true})
  elseif g == "thunder" then
    c:circle(x - 4, y - 4, 7, SOFT, {fill = true})
    c:circle(x + 4, y - 5, 6, SOFT, {fill = true})
    c:triangle(x + 1, y + 1, x + 7, y + 1, x + 2, y + 7, 0xFFD60A)
    c:triangle(x + 2, y + 6, x + 7, y + 6, x - 1, y + 12, 0xFFD60A)
  elseif g == "fog" then
    c:line(x - 9, y - 3, x + 9, y - 3, SOFT, 3)
    c:line(x - 7, y + 2, x + 9, y + 2, SOFT, 3)
    c:line(x - 9, y + 7, x + 6, y + 7, SOFT, 3)
  else -- partly / overcast: a cloud, sun peeking if partly
    if g == "partly" and not night then
      c:circle(x + 7, y - 7, 6, 0xFFD60A, {fill = true})
    end
    c:circle(x - 4, y + 1, 8, SOFT, {fill = true})
    c:circle(x + 5, y, 7, SOFT, {fill = true})
    c:rect(x - 12, y, 24, 7, SOFT)
  end
end

local function hour_label(h)
  local ap = h < 12 and "AM" or "PM"
  local h12 = h % 12
  if h12 == 0 then h12 = 12 end
  return h12 .. " " .. ap
end

-- the static content pieces, shared by the full draw and
-- by the repair pass (a piece is redrawn when a mover or
-- a cover touched its area)
local function draw_temp()
  local w = wx
  local t = math.floor(w.temp + 0.5)
  local digits = #tostring(math.abs(t))
  -- a digit is about 0.44 of the font size wide; the degree
  -- ring hugs the number's right edge
  local tw = digits * 88 + (t < 0 and 58 or 0)
  c:text(tostring(t), 233 - 16, 76, WHITE, 200, "center")
  c:circle(233 - 16 + tw / 2 + 20, 112, 14, WHITE,
           {fill = false, width = 7})
end
local function draw_condition()
  c:text(LABELS[wx.code] or "", 233, 284, WHITE, 19, "center")
end
local function draw_hum()
  c:text("Hum: " .. wx.hum .. "%", 100, 288, SOFT, 15, "center")
end
local function draw_wind()
  c:text("W: " .. math.floor(wx.wind + 0.5) .. " mph", 366,
         288, SOFT, 15, "center")
end
local function draw_hi()
  c:text("H: " .. math.floor(wx.hi + 0.5), 382, 176, WHITE,
         15, "center")
end
local function draw_lo()
  c:text("L: " .. math.floor(wx.lo + 0.5), 382, 206, SOFT,
         15, "center")
end
local function draw_column(i)
  local h = wx.hours[i]
  if not h then return end
  local cx = 133 + (i - 1) * 100
  c:text(hour_label(h.hour), cx, 324, SOFT, 13, "center")
  draw_glyph(group_of(h.code), cx, 366, not wx.is_day)
  c:text(tostring(h.temp), cx, 376, WHITE, 18, "center")
end
local function draw_dots()
  for i = 1, #CITIES do
    c:circle(203 + (i - 1) * 20, 428, 3,
             i == idx and WHITE or 0x8FA8C8, {fill = true})
  end
end

local function draw_content()
  local city = CITIES[idx]
  c:text(city.name, 233, 58, WHITE, 36, "center")
  if wx then
    draw_temp()
    draw_condition()
    -- humidity and wind flank the condition on its line;
    -- high and low stack on the right
    draw_hum()
    draw_wind()
    draw_hi()
    draw_lo()
    -- the glass hourly panel, kept inside the round face
    -- and shifted up: only the next three hours, so each
    -- column is wide and the type can breathe
    c:rect(83, 316, 300, 84, 0x0B1D38, {opacity = 105,
           radius = 16})
    local n = math.min(3, #wx.hours)
    for i = 1, n do draw_column(i) end
  else
    c:text("Loading...", 233, 220, SOFT, 20, "center")
  end
  draw_dots()
end

-- repair zones: {x1, y1, x2, y2, redraw fn}
local zones = {
  {304, 54, 432, 182, function() draw_celestial() end},
  {110, 46, 356, 102, function()
     c:text(CITIES[idx].name, 233, 58, WHITE, 36, "center")
   end},
  {100, 68, 410, 282, function() if wx then draw_temp() end end},
  {150, 278, 316, 312, function()
     if wx then draw_condition() end end},
  {40, 278, 170, 312, function() if wx then draw_hum() end end},
  {296, 278, 436, 312, function() if wx then draw_wind() end end},
  {336, 166, 428, 198, function() if wx then draw_hi() end end},
  {336, 196, 428, 228, function() if wx then draw_lo() end end},
  {83, 310, 183, 406, function()
     if wx then draw_column(1) end end},
  {183, 310, 283, 406, function()
     if wx then draw_column(2) end end},
  {283, 310, 383, 406, function()
     if wx then draw_column(3) end end},
  {193, 418, 273, 440, function() draw_dots() end},
}

local function draw_movers_all()
  local w = wx or DEFW
  local g = group_of(w.code)
  if not w.is_day and (g == "clear" or g == "partly") then
    for _, s in ipairs(stars) do draw_star(s) end
  end
  draw_celestial()
  local ccol, cop, ncloud = cloud_style(g, w.is_day)
  for i = 1, ncloud do draw_cloud(clouds[i], ccol, cop) end
  if g == "rain" or g == "thunder" then
    for _, r in ipairs(rain) do draw_rain_drop(r) end
  end
  if g == "snow" then
    for _, s in ipairs(snow) do draw_snow_flake(s) end
  end
  if flash > 0 then
    c:rect(0, 0, 466, 466, WHITE, {opacity = 110})
  end
end

local function draw_full()
  local w = wx or DEFW
  local g = group_of(w.code)
  local pal = SKY[g][w.is_day and "day" or "night"]
  draw_sky(pal)
  ui.background(pal[2])
  yoff = 0
  draw_movers_all()
  -- content, sliding up into place after a city change
  yoff = slide * slide * 90
  draw_content()
  yoff = 0
  need_full = false
end

-- one partial frame: move the movers, then repair the
-- statics their covers and new positions touched
local function draw_movers_frame()
  touches_n = 0
  local w = wx or DEFW
  local g = group_of(w.code)

  if not w.is_day and (g == "clear" or g == "partly")
     and tick % 10 == 0 then
    -- star blinks are batched twice a second: a flip
    -- anywhere in the sky widens the frame's flush region
    -- to the whole sky, so most frames should not carry
    -- one; between batches the frame is just the clouds
    for _, s in ipairs(stars) do
      -- a star only repaints when it blinks on or off;
      -- repainting every brightness step scattered tiny
      -- paints across the whole sky and merged the frame's
      -- flush into one big region
      local on = star_alpha(s) > 40
      if on ~= s.on then
        if s.on then
          paint_bg(s.x - 4, s.y - 4, 8, 8)
          touch(s.x - 4, s.y - 4, s.x + 4, s.y + 4)
        end
        draw_star(s)
        if s.on then touch(s.x - 4, s.y - 4, s.x + 4, s.y + 4) end
      end
    end
  end

  if w.is_day and (g == "clear" or g == "partly") then
    if pulse_amt() ~= prev_pulse then
      paint_bg(302, 52, 132, 132)
      touch(302, 52, 434, 184)
      draw_sun(368, 118)
    end
  end

  -- all covers first, then all clouds: the clouds drift
  -- in formation (one shared speed, so they never catch
  -- each other up), and a cover painted after a neighbour
  -- had already moved would erase part of that neighbour
  -- wherever the two overlap
  local ccol, cop, ncloud = cloud_style(g, w.is_day)
  for i = 1, ncloud do
    local cl = clouds[i]
    local nx = math.floor(cl.x + 0.5)
    if cl.dx ~= nx and cl.dx then
      -- cover with a margin: a cover that ends exactly
      -- at the shape's edge can leave a pale line of the
      -- cloud behind where the edge was antialiased
      paint_bg(cl.dx - 46 * cl.s - 5, cl.y - 30 * cl.s - 5,
               92 * cl.s + 10, 56 * cl.s + 10)
      touch(cl.dx - 46 * cl.s - 5, cl.y - 30 * cl.s - 5,
            cl.dx + 46 * cl.s + 5, cl.y + 26 * cl.s + 5)
    end
  end
  for i = 1, ncloud do
    local cl = clouds[i]
    local nx = math.floor(cl.x + 0.5)
    if cl.dx ~= nx then
      draw_cloud(cl, ccol, cop)
      touch(nx - 46 * cl.s - 5, cl.y - 30 * cl.s - 5,
            nx + 46 * cl.s + 5, cl.y + 26 * cl.s + 5)
    end
  end

  if g == "rain" or g == "thunder" then
    for _, r in ipairs(rain) do
      if r.px then
        paint_bg(r.px - 6, r.py - 2, 11, r.l + 4)
        touch(r.px - 6, r.py - 2, r.px + 5, r.py + r.l + 2)
      end
      draw_rain_drop(r)
      touch(r.x - 5, r.y, r.x + 2, r.y + r.l)
    end
  end

  if g == "snow" then
    for _, s in ipairs(snow) do
      if s.px then
        paint_bg(s.px - s.r - 3, s.py - s.r - 3,
                 2 * s.r + 6, 2 * s.r + 6)
        touch(s.px - s.r - 2, s.py - s.r - 2,
              s.px + s.r + 2, s.py + s.r + 2)
      end
      draw_snow_flake(s)
      touch(s.px - s.r, s.py - s.r, s.px + s.r, s.py + s.r)
    end
  end

  -- movers are under the panel in the full draw, so paint
  -- the panel's background back over anything that crossed
  -- it, before the panel's own contents are repaired
  if wx then
    for i = 1, touches_n do
      local t = touches[i]
      local ix1, iy1 = math.max(t[1], 83), math.max(t[2], 316)
      local ix2, iy2 = math.min(t[3], 383), math.min(t[4], 400)
      if ix2 > ix1 and iy2 > iy1 then
        paint_bg(ix1, iy1, ix2 - ix1, iy2 - iy1)
      end
    end
  end

  for _, z in ipairs(zones) do
    if touched(z[1], z[2], z[3], z[4]) then z[5]() end
  end
end

-- launcher icon: a sun coming up behind a cloud on blue
do
  local ic = ui.canvas{w = 96, h = 96, color = 0x2B66C4}
  ic:circle(62, 34, 17, 0xFFD60A, {fill = true})
  ic:circle(38, 58, 16, 0xFFFFFF, {fill = true})
  ic:circle(58, 54, 13, 0xFFFFFF, {fill = true})
  ic:rect(22, 56, 52, 12, 0xFFFFFF)
  app.icon(ic)
  ic:delete()
end

local function fetch(city)
  -- http.get takes URLs of up to 300 characters, so the
  -- request stays lean: Celsius and km/h come back and are
  -- converted here, and no forecast_days (the default week
  -- still fits the 32 KB reply cap)
  local url = "https://api.open-meteo.com/v1/forecast?latitude="
    .. city.lat .. "&longitude=" .. city.lon
    .. "&current=temperature_2m,weather_code,is_day,"
    .. "relative_humidity_2m,wind_speed_10m"
    .. "&hourly=temperature_2m,weather_code"
    .. "&daily=temperature_2m_max,temperature_2m_min"
    .. "&timezone=auto"
  local function F(cel) return math.floor(cel * 9 / 5 + 32.5) end
  http.get(url, function(ok, body)
    if not ok then
      print("weather fetch failed: " .. tostring(body):sub(1, 50))
      return
    end
    local d = json.decode(body)
    if not d or not d.current then return end
    local w = {
      temp = F(d.current.temperature_2m),
      code = d.current.weather_code,
      is_day = d.current.is_day == 1,
      hum = d.current.relative_humidity_2m,
      wind = math.floor(d.current.wind_speed_10m * 0.621 + 0.5),
      hi = F(d.daily.temperature_2m_max[1]),
      lo = F(d.daily.temperature_2m_min[1]),
      hours = {},
    }
    -- the next three hours after the current one
    local cur = d.current.time:sub(1, 13)
    local start = nil
    for i, t in ipairs(d.hourly.time) do
      if t:sub(1, 13) == cur then start = i break end
    end
    if start then
      for i = start + 1, math.min(start + 3, #d.hourly.time) do
        w.hours[#w.hours + 1] = {
          hour = tonumber(d.hourly.time[i]:sub(12, 13)),
          temp = F(d.hourly.temperature_2m[i]),
          code = d.hourly.weather_code[i],
        }
      end
    end
    save_cache(city, w)
    if CITIES[idx] == city then
      wx = w
      need_full = true
      print("weather live: " .. city.code .. " " .. w.temp ..
            "F code " .. w.code)
    end
  end)
end

wx = load_cache(CITIES[idx])
draw_full()
local gu, ga, gl = gfx.memory()
print("weather ready, on " .. CITIES[idx].code ..
      ", gfx used " .. tostring(gu) .. ", available " ..
      tostring(ga) .. ", largest " .. tostring(gl))
ui.after(800, function() fetch(CITIES[idx]) end)

local down_x, down_y = nil, nil
touch_fn = function(kind, x, y)
  if kind == "down" then
    down_x, down_y = x, y
  elseif kind == "up" and down_x then
    local dx, dy = x - down_x, y - down_y
    down_x = nil
    local moved = math.abs(dx) > 12 or math.abs(dy) > 12
    if not moved and y < 75 then
      -- tapped the city name: next city
      idx = idx % #CITIES + 1
      store.set("city", idx)
      wx = load_cache(CITIES[idx])
      slide = 1
      sound.effect("select", {volume = 25})
      fetch(CITIES[idx])
      draw_full()
    end
  end
end

-- animation: drift the clouds, fall the rain and snow,
-- twinkle the stars, and every so often, lightning. Full
-- draws happen only when the scene itself changes (city
-- slide, new data, a flash ending); every other tick is a
-- partial frame that moves the movers and repairs what
-- they touched
ui.every(50, function()
  tick = tick + 1
  for _, cl in ipairs(clouds) do
    cl.x = cl.x + cl.v
    if cl.x > 540 then cl.x = -80 end
  end
  for _, r in ipairs(rain) do
    r.y = r.y + r.v
    r.x = r.x - 1.5
    if r.y > 470 then
      r.y = -20
      r.x = (r.x * 7 + 31) % 466
    end
  end
  for _, s in ipairs(snow) do
    s.y = s.y + s.v
    if s.y > 470 then s.y = -6 end
  end
  if wx and group_of(wx.code) == "thunder" then
    if flash > 0 then flash = flash - 1
    elseif tick % 173 == 0 then flash = 3 end
  end
  local sliding = slide > 0
  if slide > 0 then slide = math.max(0, slide - 0.16) end
  if need_full or sliding then
    draw_full()
    prev_flash_on = flash > 0
  elseif flash > 0 and not prev_flash_on then
    -- the flash itself is one full-screen paint; the
    -- scene stays frozen under it for its few frames
    yoff = 0
    c:rect(0, 0, 466, 466, WHITE, {opacity = 110})
    prev_flash_on = true
  elseif flash <= 0 and prev_flash_on then
    prev_flash_on = false
    draw_full()
  else
    draw_movers_frame()
  end
end)

-- refresh the city on screen every 10 minutes
ui.every(600000, function() fetch(CITIES[idx]) end)
