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

-- the drawing surface is a 233x233 canvas at zoom 2 (106 KB
-- instead of 424 KB at full size). All drawing code works
-- in 466 screen space; this proxy halves coordinates and
-- sizes into canvas pixels. yoff slides the content (not
-- the sky) during a city change.
local touch_fn = nil
local raw = ui.canvas{
  w = 233, h = 233, zoom = 2, color = 0x101D3D,
  on_touch = function(kind, x, y)
    if touch_fn then touch_fn(kind, x, y) end
  end,
}
local yoff = 0
local function px(v) return v / 2 end
local function py(v) return (v + yoff) / 2 end
local function half(v) return v / 2 end
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
  {x = 395, y = 170, s = 0.65, v = 0.5},
  {x = 120, y = 215, s = 0.55, v = 0.25},
  {x = 330, y = 228, s = 0.5, v = 0.42},
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

local function draw_sky(pal)
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

local function draw_stars()
  for _, s in ipairs(stars) do
    local a = 120 + 110 * math.sin(tick * 0.06 + s.p)
    if a > 40 then
      c:circle(s.x, s.y, 2, WHITE, {fill = true, opacity = a})
    end
  end
end

local function draw_sun(x, y)
  local pulse = 4 * math.sin(tick * 0.03)
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

local function draw()
  local city = CITIES[idx]
  local w = wx or {temp = 0, code = 1, is_day = true, hi = 0,
                   lo = 0, hum = 0, wind = 0, hours = {}}
  local g = group_of(w.code)
  local pal = SKY[g][w.is_day and "day" or "night"]
  draw_sky(pal)
  ui.background(pal[2])

  -- the living sky (not slid by the city-change animation)
  yoff = 0
  if not w.is_day and (g == "clear" or g == "partly") then
    draw_stars()
  end
  if g == "clear" or g == "partly" then
    if w.is_day then draw_sun(368, 118)
    else draw_moon(368, 118, pal[2]) end
  end
  local cloud_col = WHITE
  local cloud_op = w.is_day and 205 or 110
  if g == "overcast" or g == "thunder" then
    cloud_col, cloud_op = 0x9AA7B5, 225
  elseif g == "rain" then
    cloud_col, cloud_op = 0xB9C7D4, 215
  elseif g == "fog" then
    cloud_col, cloud_op = 0xC7D0D8, 160
  end
  local ncloud = (g == "clear") and 1 or (g == "partly") and 3 or 4
  for i = 1, ncloud do draw_cloud(clouds[i], cloud_col, cloud_op) end
  if g == "rain" or g == "thunder" then
    for _, r in ipairs(rain) do
      c:line(r.x, r.y, r.x - 3, r.y + r.l, 0xAFD4F5, 2)
    end
  end
  if g == "snow" then
    for _, s in ipairs(snow) do
      c:circle(s.x + 8 * math.sin(tick * 0.05 + s.p), s.y, s.r,
               WHITE, {fill = true, opacity = 220})
    end
  end
  if flash > 0 then
    c:rect(0, 0, 466, 466, WHITE, {opacity = 110})
  end

  -- content, sliding up into place after a city change
  yoff = slide * slide * 90
  c:text(city.name, 233, 58, WHITE, 36, "center")
  if wx then
    local t = math.floor(w.temp + 0.5)
    local digits = #tostring(math.abs(t))
    local tw = digits * 62 + (t < 0 and 34 or 0)
    c:text(tostring(t), 233 - 16, 128, WHITE, 118, "center")
    c:circle(233 - 16 + tw / 2 + 16, 146, 10, WHITE,
             {fill = false, width = 5})
    c:text(LABELS[w.code] or "", 233, 237, WHITE, 21, "center")
    -- high and low stacked on the right of the temperature,
    -- wind and humidity stacked on the left
    c:text("H: " .. math.floor(w.hi + 0.5), 382, 176, WHITE,
           15, "center")
    c:text("L: " .. math.floor(w.lo + 0.5), 382, 206, SOFT,
           15, "center")
    c:text("Wind " .. math.floor(w.wind + 0.5) .. " mph", 84,
           176, SOFT, 13, "center")
    c:text("Humidity " .. w.hum .. "%", 84, 206, SOFT, 13,
           "center")
    -- the glass hourly panel, kept inside the round face
    -- and shifted up: only the next three hours, so each
    -- column is wide and the type can breathe
    c:rect(83, 306, 300, 88, 0x0B1D38, {opacity = 105,
           radius = 16})
    c:line(99, 336, 367, 336, WHITE, 1)
    local n = math.min(3, #w.hours)
    for i = 1, n do
      local cx = 133 + (i - 1) * 100
      local h = w.hours[i]
      c:text(hour_label(h.hour), cx, 314,
             SOFT, 13, "center")
      draw_glyph(group_of(h.code), cx, 356, not w.is_day)
      c:text(tostring(h.temp), cx, 368, WHITE, 18, "center")
    end
  else
    c:text("Loading...", 233, 220, SOFT, 20, "center")
  end
  -- city dots
  for i = 1, #CITIES do
    c:circle(203 + (i - 1) * 20, 428, 3,
             i == idx and WHITE or 0x8FA8C8, {fill = true})
  end
  yoff = 0
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
      print("weather live: " .. city.code .. " " .. w.temp ..
            "F code " .. w.code)
    end
  end)
end

wx = load_cache(CITIES[idx])
draw()
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
      draw()
    end
  end
end

-- animation: drift the clouds, fall the rain and snow,
-- twinkle the stars, and every so often, lightning
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
  if slide > 0 then slide = math.max(0, slide - 0.16) end
  draw()
end)

-- refresh the city on screen every 10 minutes
ui.every(600000, function() fetch(CITIES[idx]) end)
