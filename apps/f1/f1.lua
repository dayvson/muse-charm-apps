-- Grand Prix: Formula 1 on the gadget. Four screens: the
-- next race (a big circuit outline with a red dot lapping
-- it, the start in local time, and a ring of segments near
-- the edge of the screen that drains over the last seven
-- days before lights out), the weekend schedule, the last
-- race's podium, and the championship top 3 with flags and
-- team colours. Tapping the circuit on the race screen
-- swaps it for the race after the next one, and back. If
-- the app is open at lights out, the five start lights run
-- their sequence. Swipe or tap to move between screens.
-- Data comes from the free, keyless Ergast mirror at
-- api.jolpi.ca and is cached in the store, so the app opens
-- instantly and works offline with saved data.

local BG = 0x0B0B10
local PALE = 0xD8D5CC
local LAV = 0x8E8CA8
local GOLD = 0xD9A441
local RED = 0xE10600
local DIM = 0x3A3A48
local ROAD = 0x2E2E3A
local CAR = 0xFF3B2F
local TRAIL = {0xC10600, 0x7A0300, 0x470200}

local MONTHS = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"}
local WDAYS = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"}

-- simplified circuit outlines, traced by hand, points in a
-- 0..100 box (y down), drawn as a closed loop
local OUTLINES = {
  vegas = {{14,30},{34,16},{58,12},{80,20},{90,36},{80,48},{90,62},
           {76,78},{54,72},{38,84},{20,76},{10,60},{20,46}},
  losail = {{6,58},{26,50},{44,54},{56,42},{48,30},{58,16},{74,20},
            {88,34},{80,50},{90,64},{74,80},{52,74},{32,84},{14,72}},
  yas_marina = {{88,22},{66,32},{52,26},{36,36},{26,28},{12,38},
                {20,52},{8,62},{20,74},{36,68},{48,80},{64,72},
                {76,82},{88,66},{78,50},{90,38}},
}

-- flags drawn from stripe recipes: v/h thirds, h2 halves, or a
-- named special (crosses, discs, the Union Jack)
local FLAGS = {
  Italian = {"v", 0x009246, 0xF4F5F0, 0xCE2B37},
  French = {"v", 0x0055A4, 0xF4F5F0, 0xEF4135},
  Mexican = {"v", 0x006847, 0xF4F5F0, 0xCE1126},
  Dutch = {"h", 0xAE1C28, 0xF4F5F0, 0x21468B},
  German = {"h", 0x151515, 0xDD0000, 0xFFCC00},
  Spanish = {"h", 0xAA151B, 0xF1BF00, 0xAA151B},
  Austrian = {"h", 0xED2939, 0xF4F5F0, 0xED2939},
  Argentine = {"h", 0x74ACDF, 0xF4F5F0, 0x74ACDF},
  Monegasque = {"h2", 0xF4F5F0, 0xCE1126},
  British = {"uk"}, Finnish = {"fi"}, Danish = {"dk"},
  Swiss = {"ch"}, Japanese = {"jp"}, American = {"us"},
  Australian = {"au"}, Brazilian = {"br"}, Canadian = {"ca"},
  Thai = {"th"}, Chinese = {"cn"},
}

local TEAM_COLORS = {
  Mercedes = 0x27F4D2, ["Red Bull"] = 0x3671C6,
  Ferrari = 0xE8002D, McLaren = 0xFF8000,
  ["Aston Martin"] = 0x229971, Alpine = 0x0093CC,
  Williams = 0x64C4FF, Haas = 0xB6BABD, Audi = 0xBB0A30,
  ["Racing Bulls"] = 0x6692FF, ["RB F1"] = 0x6692FF,
}

local function team_color(team)
  for k, v in pairs(TEAM_COLORS) do
    if team and team:find(k, 1, true) then return v end
  end
  return DIM
end

-- The drawing surface is one full-screen 466x466 canvas, so
-- the countdown segments can sit near the true edge of the
-- round screen. All the drawing code works in a 352 box
-- centred on it; this proxy shifts every coordinate by 57
-- to land there. The canvas is created first, before
-- anything else allocates.
local touch_fn = nil
local raw = ui.canvas{
  w = 466, h = 466, color = BG,
  on_touch = function(kind, x, y)
    if touch_fn then touch_fn(kind, x, y) end
  end,
}
local c = {}
function c:clear(col) raw:clear(col) end
function c:rect(x, y, w, h, col, o)
  raw:rect(x + 57, y + 57, w, h, col, o) end
function c:circle(x, y, r, col, o)
  raw:circle(x + 57, y + 57, r, col, o) end
function c:line(x1, y1, x2, y2, col, w)
  raw:line(x1 + 57, y1 + 57, x2 + 57, y2 + 57, col, w) end
function c:triangle(x1, y1, x2, y2, x3, y3, col)
  raw:triangle(x1 + 57, y1 + 57, x2 + 57, y2 + 57,
               x3 + 57, y3 + 57, col) end
function c:text(s, x, y, col, size, align)
  raw:text(s, x + 57, y + 57, col, size, align) end
function c:arc(x, y, r, a0, sweep, col, w)
  raw:arc(x + 57, y + 57, r, a0, sweep, col, w) end

local race = nil   -- {name, cid, circuit, loc, country, epoch}
local after = nil  -- the race after the next one, same shape
local after_mode = false -- race screen showing `after`
local next_round = nil
local top = {}     -- up to 3 {code, name, team, pts, wins, nat}
local sched = {}   -- {{label, epoch}, ...} the race weekend
local last = nil   -- {name, results = {top 3 like `top`}}
local season, round = nil, nil
local state = "offline" -- live, saved, offline
local screen = 1
local path = nil   -- {pts, cum, total, cid} for the lap animation
local anim_dist = 0
local lights = 0   -- start-lights sequence tick, 0 = not running

do
  local rn = store.get("rname")
  if rn then
    race = {name = rn, cid = store.get("rcid"),
            circuit = store.get("rcircuit"), loc = store.get("rloc"),
            country = store.get("rctry"), epoch = store.get("repoch")}
    state = "saved"
  end
  for i = 1, 3 do
    local s = store.get("top" .. i)
    if s then
      local code, name, team, pts, wins, nat =
        s:match("^(.-)|(.-)|(.-)|(.-)|(.-)|(.*)$")
      if not code then
        code, name, team, pts, wins =
          s:match("^(.-)|(.-)|(.-)|(.-)|(.+)$")
        nat = ""
      end
      if code then
        top[i] = {code = code, name = name, team = team,
                  pts = tonumber(pts), wins = tonumber(wins),
                  nat = nat}
      end
    end
  end
  season = store.get("season")
  round = store.get("round")
  local sc = store.get("sched")
  if sc then
    for label, ep in sc:gmatch("([^;:]+):(%d+)") do
      sched[#sched + 1] = {label = label, epoch = tonumber(ep)}
    end
  end
  local an = store.get("aname")
  if an then
    after = {name = an, cid = store.get("acid"),
             epoch = store.get("aepoch")}
  end
  local ln = store.get("lname")
  if ln then
    last = {name = ln, results = {}}
    local ll = store.get("lloc")
    if ll and ll ~= "" then last.loc = ll end
    local lr = store.get("lres")
    if lr then
      for code, name, team, nat in
          lr:gmatch("([^|;]+)|([^|;]+)|([^|;]+)|([^;]*)") do
        last.results[#last.results + 1] =
          {code = code, name = name, team = team, nat = nat}
      end
    end
  end
end

-- date math (no os library on the gadget): civil conversions
local function days_from_civil(y, m, d)
  local yy = (m <= 2) and (y - 1) or y
  local era = math.floor(yy / 400)
  local yoe = yy - era * 400
  local mp = (m > 2) and (m - 3) or (m + 9)
  local doy = math.floor((153 * mp + 2) / 5) + d - 1
  local doe = yoe * 365 + math.floor(yoe / 4)
              - math.floor(yoe / 100) + doy
  return era * 146097 + doe - 719468
end

local function civil_from_days(z)
  z = z + 719468
  local era = math.floor(z / 146097)
  local doe = z - era * 146097
  local yoe = math.floor((doe - math.floor(doe / 1460)
    + math.floor(doe / 36524) - math.floor(doe / 146096)) / 365)
  local y = yoe + era * 400
  local doy = doe - (365 * yoe + math.floor(yoe / 4)
                     - math.floor(yoe / 100))
  local mp = math.floor((5 * doy + 2) / 153)
  local d = doy - math.floor((153 * mp + 2) / 5) + 1
  local m = (mp < 10) and (mp + 3) or (mp - 9)
  if m <= 2 then y = y + 1 end
  return y, m, d
end

local function local_offset()
  local d = time.date()
  local as_utc = days_from_civil(d.year, d.month, d.day) * 86400
                 + d.hour * 3600 + d.min * 60 + (d.sec or 0)
  local off = as_utc - time.now()
  return math.floor((off + 450) / 900) * 900
end

local function race_local_parts(epoch)
  local t = epoch + local_offset()
  local days = math.floor(t / 86400)
  local secs = t - days * 86400
  local y, mo, d = civil_from_days(days)
  local wday = WDAYS[(days + 4) % 7 + 1]
  local hh = math.floor(secs / 3600)
  local mm = math.floor((secs % 3600) / 60)
  local ampm = hh < 12 and "AM" or "PM"
  local h12 = hh % 12
  if h12 == 0 then h12 = 12 end
  return wday .. ", " .. MONTHS[mo] .. " " .. d .. " - " ..
         h12 .. ":" .. string.format("%02d", mm) .. " " .. ampm
end

local function parse_dt(date, time_s)
  if not date then return nil end
  local y, mo, d = date:match("^(%d+)-(%d+)-(%d+)$")
  if not y then return nil end
  local hh, mi = (time_s or "00:00:00Z"):match("^(%d+):(%d+)")
  return days_from_civil(tonumber(y), tonumber(mo), tonumber(d))
         * 86400 + tonumber(hh or 0) * 3600 + tonumber(mi or 0) * 60
end

local function fmt_short(epoch)
  local t = epoch + local_offset()
  local days = math.floor(t / 86400)
  local secs = t - days * 86400
  local hh = math.floor(secs / 3600)
  local mm = math.floor((secs % 3600) / 60)
  local ampm = hh < 12 and "AM" or "PM"
  local h12 = hh % 12
  if h12 == 0 then h12 = 12 end
  return WDAYS[(days + 4) % 7 + 1] .. " " .. h12 .. ":" ..
         string.format("%02d", mm) .. " " .. ampm
end

local function countdown(epoch)
  local diff = epoch - time.now()
  if diff <= 0 then return "the race is on" end
  local days = math.floor(diff / 86400)
  local hrs = math.floor((diff % 86400) / 3600)
  local min = math.floor((diff % 3600) / 60)
  if days > 0 then return "in " .. days .. "d " .. hrs .. "h" end
  if hrs > 0 then return "in " .. hrs .. "h " .. min .. "m" end
  return "in " .. min .. "m"
end

-- the gadget fonts are ASCII only: fold accented letters in
-- names the API returns (Sao Paulo, Autodromo) to plain ones
local function ascii_fold(s)
  if not s then return s end
  s = s:gsub("\195\160", "a"):gsub("\195\161", "a")
       :gsub("\195\162", "a"):gsub("\195\163", "a")
       :gsub("\195\168", "e"):gsub("\195\169", "e")
       :gsub("\195\170", "e"):gsub("\195\173", "i")
       :gsub("\195\179", "o"):gsub("\195\180", "o")
       :gsub("\195\186", "u"):gsub("\195\188", "u")
       :gsub("\195\167", "c"):gsub("\195\177", "n")
  return s
end

local function draw_flag(x, y, nat)
  local w, h = 38, 25
  local spec = FLAGS[nat]
  c:rect(x - 1, y - 1, w + 2, h + 2, 0x000000)
  if not spec then
    c:rect(x, y, w, h, 0x55555F)
    return
  end
  local t = spec[1]
  if t == "v" then
    local tw = w / 3
    c:rect(x, y, tw + 1, h, spec[2])
    c:rect(x + tw, y, tw + 1, h, spec[3])
    c:rect(x + 2 * tw, y, w - 2 * tw, h, spec[4])
  elseif t == "h" then
    local th = h / 3
    c:rect(x, y, w, th + 1, spec[2])
    c:rect(x, y + th, w, th + 1, spec[3])
    c:rect(x, y + 2 * th, w, h - 2 * th, spec[4])
  elseif t == "h2" then
    c:rect(x, y, w, h / 2 + 1, spec[2])
    c:rect(x, y + h / 2, w, h - h / 2, spec[3])
  elseif t == "uk" then
    c:rect(x, y, w, h, 0x012169)
    c:line(x, y, x + w, y + h, 0xF4F5F0, 3)
    c:line(x + w, y, x, y + h, 0xF4F5F0, 3)
    c:rect(x + w / 2 - 5, y, 10, h, 0xF4F5F0)
    c:rect(x, y + h / 2 - 4, w, 8, 0xF4F5F0)
    c:rect(x + w / 2 - 3, y, 6, h, 0xC8102E)
    c:rect(x, y + h / 2 - 2, w, 5, 0xC8102E)
  elseif t == "fi" then
    c:rect(x, y, w, h, 0xF4F5F0)
    c:rect(x + 10, y, 6, h, 0x002F6C)
    c:rect(x, y + 9, w, 6, 0x002F6C)
  elseif t == "dk" then
    c:rect(x, y, w, h, 0xC8102E)
    c:rect(x + 10, y, 5, h, 0xF4F5F0)
    c:rect(x, y + 10, w, 5, 0xF4F5F0)
  elseif t == "ch" then
    c:rect(x, y, w, h, 0xDA291C)
    c:rect(x + w / 2 - 3, y + 5, 6, h - 10, 0xF4F5F0)
    c:rect(x + w / 2 - 8, y + h / 2 - 3, 16, 6, 0xF4F5F0)
  elseif t == "jp" then
    c:rect(x, y, w, h, 0xF4F5F0)
    c:circle(x + w / 2, y + h / 2, 6, 0xBC002D, {fill = true})
  elseif t == "us" then
    c:rect(x, y, w, h, 0xF4F5F0)
    for i = 0, 3 do c:rect(x, y + i * 7, w, 4, 0xB22234) end
    c:rect(x, y, 17, 13, 0x3C3B6E)
    c:circle(x + 4, y + 4, 1, 0xF4F5F0, {fill = true})
    c:circle(x + 9, y + 4, 1, 0xF4F5F0, {fill = true})
    c:circle(x + 13, y + 4, 1, 0xF4F5F0, {fill = true})
    c:circle(x + 6, y + 9, 1, 0xF4F5F0, {fill = true})
    c:circle(x + 11, y + 9, 1, 0xF4F5F0, {fill = true})
  elseif t == "au" then
    c:rect(x, y, w, h, 0x00247D)
    c:rect(x, y, 17, 12, 0x012169)
    c:rect(x + 7, y, 3, 12, 0xF4F5F0)
    c:rect(x, y + 5, 17, 3, 0xF4F5F0)
    c:circle(x + 26, y + 7, 2, 0xF4F5F0, {fill = true})
    c:circle(x + 31, y + 15, 2, 0xF4F5F0, {fill = true})
    c:circle(x + 24, y + 19, 2, 0xF4F5F0, {fill = true})
    c:circle(x + 9, y + 19, 2, 0xF4F5F0, {fill = true})
  elseif t == "br" then
    c:rect(x, y, w, h, 0x009C3B)
    c:triangle(x + 6, y + h / 2, x + w / 2, y + 3, x + w - 6,
               y + h / 2, 0xFFDF00)
    c:triangle(x + 6, y + h / 2, x + w / 2, y + h - 3, x + w - 6,
               y + h / 2, 0xFFDF00)
    c:circle(x + w / 2, y + h / 2, 5, 0x002776, {fill = true})
  elseif t == "ca" then
    c:rect(x, y, w, h, 0xF4F5F0)
    c:rect(x, y, 9, h, 0xFF0000)
    c:rect(x + w - 9, y, 9, h, 0xFF0000)
    c:circle(x + w / 2, y + h / 2, 4, 0xFF0000, {fill = true})
  elseif t == "th" then
    c:rect(x, y, w, h, 0xA51931)
    c:rect(x, y + 4, w, 17, 0xF4F5F0)
    c:rect(x, y + 8, w, 9, 0x2D2A4A)
  elseif t == "cn" then
    c:rect(x, y, w, h, 0xDE2910)
    c:circle(x + 6, y + 6, 2, 0xFFDE00, {fill = true})
    c:circle(x + 12, y + 4, 1, 0xFFDE00, {fill = true})
    c:circle(x + 13, y + 8, 1, 0xFFDE00, {fill = true})
    c:circle(x + 12, y + 12, 1, 0xFFDE00, {fill = true})
  end
end

local function build_path(cid)
  local raw = OUTLINES[cid]
  if not raw then path = nil return end
  local cx, cy, hx, hy = 176, 190, 140, 104
  local pts, cum, total = {}, {}, 0
  for i, p in ipairs(raw) do
    pts[i] = {cx + (p[1] - 50) / 50 * hx,
              cy + (p[2] - 50) / 50 * hy}
  end
  for i = 1, #pts do
    local a, b = pts[i], pts[i % #pts + 1]
    local dx, dy = b[1] - a[1], b[2] - a[2]
    total = total + math.sqrt(dx * dx + dy * dy)
    cum[i] = total
  end
  path = {pts = pts, cum = cum, total = total, cid = cid}
end

local function point_at(d)
  d = d % path.total
  for i = 1, #path.pts do
    if d <= path.cum[i] then
      local prev = (i == 1) and 0 or path.cum[i - 1]
      local a = path.pts[i]
      local b = path.pts[i % #path.pts + 1]
      local seg = path.cum[i] - prev
      local f = (seg == 0) and 0 or (d - prev) / seg
      return a[1] + (b[1] - a[1]) * f, a[2] + (b[2] - a[2]) * f
    end
  end
  return path.pts[1][1], path.pts[1][2]
end

local function draw_outline(cid)
  if not OUTLINES[cid] then
    -- no tracing for this circuit yet: a location pin instead
    c:circle(176, 164, 30, GOLD, {fill = false, width = 4})
    c:circle(176, 164, 10, GOLD, {fill = true})
    c:triangle(158, 182, 194, 182, 176, 218, GOLD)
    return
  end
  if not path or path.cid ~= cid then build_path(cid) end
  for pass = 1, 2 do
    local col = (pass == 1) and ROAD or PALE
    local wdt = (pass == 1) and 11 or 2
    for i = 1, #path.pts do
      local a = path.pts[i]
      local b = path.pts[i % #path.pts + 1]
      c:line(a[1], a[2], b[1], b[2], col, wdt)
    end
  end
  c:circle(path.pts[1][1], path.pts[1][2], 3, 0xFFFFFF,
           {fill = true})
  for k = 3, 1, -1 do
    local x, y = point_at(anim_dist - k * 7)
    c:circle(x, y, 5 - k, TRAIL[k], {fill = true})
  end
  local hx, hy = point_at(anim_dist)
  c:circle(hx, hy, 5, CAR, {fill = true})
end

-- a segmented ring near the edge of the screen: 48 segments,
-- lit from the top clockwise for the share of the last seven
-- days still to go before lights out; gold, then red inside
-- the last day, and fully red while the race is on
local function draw_segs(epoch)
  if not epoch then return end
  local diff = epoch - time.now()
  local frac, col
  if diff <= 0 then
    frac, col = 1, RED
  else
    frac = diff / (7 * 86400)
    if frac > 1 then frac = 1 end
    col = (diff <= 86400) and RED or GOLD
  end
  local n = 48
  local lit = math.ceil(frac * n)
  for i = 0, n - 1 do
    local a = -math.pi / 2 + (i + 0.5) * 2 * math.pi / n
    local x = 176 + 220 * math.cos(a)
    local y = 176 + 220 * math.sin(a)
    c:rect(x - 3, y - 3, 6, 6, (i < lit) and col or 0x1C1C24)
  end
end

local function draw_race()
  local disp = (after_mode and after) or race
  c:text(after_mode and "THE ONE AFTER" or "NEXT RACE",
         176, 32, LAV, 13, "center")
  if not disp then
    c:text("No race data yet", 176, 150, PALE, 20, "center")
    c:text("check your connection", 176, 182, LAV, 14, "center")
    return
  end
  local head = disp.name:gsub(" Grand Prix$", "")
  c:text(head, 176, 48, PALE, 30, "center")
  c:text("GRAND PRIX", 176, 80, GOLD, 15, "center")
  draw_segs(disp.epoch)
  draw_outline(disp.cid)
  c:text(race_local_parts(disp.epoch), 176, 268, PALE, 17, "center")
  c:text(countdown(disp.epoch) .. "  -  " .. state, 176, 292,
         GOLD, 13, "center")
end

local function draw_schedule()
  c:text("WEEKEND SCHEDULE", 176, 34, LAV, 14, "center")
  if race then
    local head = race.name:gsub(" Grand Prix$", "")
    c:text(head, 176, 54, PALE, 24, "center")
  end
  c:line(96, 88, 256, 88, DIM, 1)
  if #sched == 0 then
    c:text("No schedule yet", 176, 160, PALE, 18, "center")
    return
  end
  local now = time.now()
  local next_i = nil
  for i, s in ipairs(sched) do
    if s.epoch > now and not next_i then next_i = i end
  end
  for i, s in ipairs(sched) do
    local y = 102 + (i - 1) * 38
    local col = (s.epoch <= now) and DIM
                or ((i == next_i) and GOLD or PALE)
    if i == next_i then
      c:circle(50, y + 9, 4, GOLD, {fill = true})
    end
    c:text(s.label, 66, y, col, 19, "left")
    c:text(fmt_short(s.epoch), 286, y, col, 19, "right")
  end
end

local function draw_podium()
  c:text("LAST RACE", 176, 40, LAV, 13, "center")
  if not last or #last.results == 0 then
    c:text("No result yet", 176, 160, PALE, 18, "center")
    return
  end
  -- the API still calls Interlagos the Brazilian Grand Prix,
  -- its pre-2021 name; the city is the name fans use now
  local head = (last.loc and last.loc ~= ""
                and ascii_fold(last.loc))
               or last.name:gsub(" Grand Prix$", "")
  c:text(head, 176, 58, PALE, 22, "center")
  local medals = {GOLD, 0xC0C0C0, 0xCD7F32}
  for i, d in ipairs(last.results) do
    local y = 104 + (i - 1) * 56
    c:rect(44, y, 5, 40, team_color(d.team))
    draw_flag(56, y + 4, d.nat)
    c:text(i .. ". " .. d.code .. " - " .. d.name, 104, y + 2,
           medals[i], 19, "left")
    c:text(d.team, 104, y + 27, LAV, 13, "left")
  end
  c:text("the podium from the last race", 176, 288, LAV, 13,
         "center")
end

-- the start-lights sequence: five red lights come on one by
-- one, hold, then go out together under a GO flash
local function draw_lights()
  local head = race and race.name:gsub(" Grand Prix$", "")
               or "GRAND PRIX"
  c:text(head:upper(), 176, 62, PALE, 24, "center")
  c:text("LIGHTS OUT", 176, 92, LAV, 13, "center")
  local lit
  if lights <= 40 then lit = math.ceil(lights / 8)
  elseif lights <= 48 then lit = 5
  else lit = 0 end
  for i = 1, 5 do
    local x = 176 + (i - 3) * 46
    if lights > 48 then
      c:circle(x, 176, 17, 0x2BD96A, {fill = true})
    elseif i <= lit then
      c:circle(x, 176, 17, CAR, {fill = true})
    else
      c:circle(x, 176, 17, DIM, {fill = false, width = 3})
    end
  end
  if lights > 48 then
    c:text("GO GO GO", 176, 226, GOLD, 28, "center")
  end
end

-- a small gold trophy, drawn from shapes
local function draw_trophy(x, y)
  c:rect(x, y, 26, 4, GOLD)
  c:triangle(x, y + 4, x + 26, y + 4, x + 19, y + 19, GOLD)
  c:triangle(x, y + 4, x + 19, y + 19, x + 7, y + 19, GOLD)
  c:circle(x - 3, y + 9, 5, GOLD, {fill = false, width = 2})
  c:circle(x + 29, y + 9, 5, GOLD, {fill = false, width = 2})
  c:rect(x + 11, y + 19, 4, 6, GOLD)
  c:rect(x + 5, y + 25, 16, 3, GOLD)
end

local function draw_standings()
  draw_trophy(84, 26)
  c:text("CHAMPIONSHIP", 196, 30, PALE, 20, "center")
  if season then
    c:text(season .. " - after round " .. round, 176, 64, GOLD, 14,
           "center")
  end
  if #top == 0 then
    c:text("No standings yet", 176, 150, PALE, 20, "center")
    return
  end
  for i, d in ipairs(top) do
    local y = 96 + (i - 1) * 62
    c:rect(44, y, 5, 42, team_color(d.team))
    draw_flag(56, y + 4, d.nat)
    c:text(i .. ". " .. d.code .. " - " .. d.name, 104, y + 2,
           i == 1 and GOLD or PALE, 19, "left")
    c:text(d.team .. " - " .. d.pts .. " pts - " .. d.wins ..
           (d.wins == 1 and " win" or " wins"), 104, y + 28,
           LAV, 13, "left")
    if i < #top then c:line(44, y + 52, 308, y + 52, DIM, 1) end
  end
  c:text("top 3 in the title race", 176, 288, LAV, 13, "center")
end

local function draw()
  c:clear(BG)
  if lights > 0 then
    draw_lights()
    return
  end
  if screen == 1 then draw_race()
  elseif screen == 2 then draw_schedule()
  elseif screen == 3 then draw_podium()
  else draw_standings() end
  for i = 1, 4 do
    c:circle(146 + (i - 1) * 20, 330, 3,
             i == screen and PALE or DIM, {fill = true})
  end
end

-- launcher icon: the F1 mark, drawn in red on near-black: a
-- slanted F whose top bar flows into the 1 beside it
do
  local ic = ui.canvas{w = 96, h = 96, color = 0x0B0B10}
  local function quad(x1, y1, x2, y2, x3, y3, x4, y4)
    ic:triangle(x1, y1, x2, y2, x3, y3, RED)
    ic:triangle(x1, y1, x3, y3, x4, y4, RED)
  end
  quad(30, 22, 42, 22, 34, 74, 22, 74)   -- F stem
  quad(30, 22, 82, 22, 75, 34, 37, 34)   -- F top bar
  quad(36, 42, 64, 42, 59, 53, 32, 53)   -- F mid bar
  quad(74, 22, 86, 22, 78, 74, 66, 74)   -- the 1
  ic:triangle(74, 22, 62, 30, 72, 34, RED) -- the 1's flag
  app.icon(ic)
  ic:delete()
end

ui.background(BG)

local down_x, down_y = nil, nil
touch_fn = function(kind, x, y)
  if kind == "down" then
    down_x, down_y = x, y
  elseif kind == "up" and down_x then
    local dx = x - down_x
    down_x = nil
    if dx < -40 then
      screen = screen % 4 + 1
      after_mode = false
    elseif dx > 40 then
      screen = (screen + 2) % 4 + 1
      after_mode = false
    elseif math.abs(dx) <= 12 then
      -- a tap on the circuit (raw screen coords: the circuit
      -- sits around 233, 247 with radius about 130) swaps the
      -- race screen to the race after the next one, and back
      local cx, cy = x - 233, y - 247
      if screen == 1 and after
         and cx * cx + cy * cy < 130 * 130 then
        after_mode = not after_mode
      else
        screen = screen % 4 + 1
        after_mode = false
      end
    else return end
    sound.effect("select", {volume = 25})
    draw()
  end
end

draw()
print("f1 ready, screen " .. screen .. ", data " .. state)

local function save_race(r)
  race = r
  store.set("rname", r.name)
  store.set("rcid", r.cid)
  store.set("rcircuit", r.circuit)
  store.set("rloc", r.loc)
  store.set("rctry", r.country)
  store.set("repoch", r.epoch)
end

local function fetch_after()
  if not next_round then return end
  http.get("https://api.jolpi.ca/ergast/f1/current/" ..
           (next_round + 1) .. ".json", function(ok, body)
    if ok then
      local data = json.decode(body)
      local r = data and data.MRData and data.MRData.RaceTable
                and data.MRData.RaceTable.Races[1]
      if r then
        after = {name = r.raceName, cid = r.Circuit.circuitId,
                 epoch = parse_dt(r.date, r.time)}
        store.set("aname", after.name)
        store.set("acid", after.cid)
        store.set("aepoch", after.epoch)
        print("f1 after next: " .. r.raceName)
        return
      end
    end
    print("f1 after fetch failed: " .. tostring(body):sub(1, 60))
  end)
end

local function fetch_last()
  http.get("https://api.jolpi.ca/ergast/f1/current/last/" ..
           "results.json", function(ok, body)
    if ok then
      local data = json.decode(body)
      local r = data and data.MRData and data.MRData.RaceTable
                and data.MRData.RaceTable.Races[1]
      if r and r.Results then
        last = {name = r.raceName,
                loc = r.Circuit.Location.locality, results = {}}
        local str = ""
        for i = 1, math.min(3, #r.Results) do
          local e = r.Results[i]
          local d = {code = e.Driver.code,
                     name = e.Driver.familyName,
                     team = e.Constructor.name,
                     nat = e.Driver.nationality}
          last.results[i] = d
          if i > 1 then str = str .. ";" end
          str = str .. d.code .. "|" .. d.name .. "|" .. d.team ..
                "|" .. (d.nat or "")
        end
        store.set("lname", last.name)
        store.set("lloc", last.loc or "")
        store.set("lres", str)
        draw()
        print("f1 last race: " .. r.raceName)
        fetch_after()
        return
      end
    end
    print("f1 last fetch failed: " .. tostring(body):sub(1, 60))
    fetch_after()
  end)
end

local function fetch_standings()
  http.get("https://api.jolpi.ca/ergast/f1/current/" ..
           "driverStandings.json", function(ok, body)
    if ok then
      local data = json.decode(body)
      local list = data and data.MRData and data.MRData.StandingsTable
                   and data.MRData.StandingsTable.StandingsLists[1]
      if list then
        season = list.season
        round = list.round
        store.set("season", season)
        store.set("round", round)
        top = {}
        for i = 1, math.min(3, #list.DriverStandings) do
          local e = list.DriverStandings[i]
          local d = {code = e.Driver.code, name = e.Driver.familyName,
                     team = e.Constructors[1].name,
                     pts = tonumber(e.points), wins = tonumber(e.wins),
                     nat = e.Driver.nationality}
          top[i] = d
          store.set("top" .. i, d.code .. "|" .. d.name .. "|" ..
                    d.team .. "|" .. d.pts .. "|" .. d.wins .. "|" ..
                    (d.nat or ""))
        end
        state = "live"
        draw()
        print("f1 standings updated, round " .. round)
        fetch_last()
        return
      end
    end
    print("f1 standings fetch failed: " .. tostring(body):sub(1, 60))
    fetch_last()
  end)
end

local fetched = false
local function fetch_race()
  http.get("https://api.jolpi.ca/ergast/f1/current/next.json",
           function(ok, body)
    if ok then
      local data = json.decode(body)
      local r = data and data.MRData and data.MRData.RaceTable
                and data.MRData.RaceTable.Races[1]
      if r then
        local race_epoch = parse_dt(r.date, r.time)
        next_round = tonumber(r.round)
        save_race({
          name = r.raceName, cid = r.Circuit.circuitId,
          circuit = r.Circuit.circuitName,
          loc = r.Circuit.Location.locality,
          country = r.Circuit.Location.country,
          epoch = race_epoch,
        })
        local sess = {}
        local function add_s(label, obj)
          if obj then
            local ep = parse_dt(obj.date, obj.time)
            if ep then sess[#sess + 1] = {label = label, epoch = ep} end
          end
        end
        add_s("FP1", r.FirstPractice)
        add_s("FP2", r.SecondPractice)
        add_s("FP3", r.ThirdPractice)
        add_s("SPRINT", r.Sprint)
        add_s("QUALI", r.Qualifying)
        sess[#sess + 1] = {label = "RACE", epoch = race_epoch}
        for i = 2, #sess do
          local j = i
          while j > 1 and sess[j - 1].epoch > sess[j].epoch do
            sess[j], sess[j - 1] = sess[j - 1], sess[j]
            j = j - 1
          end
        end
        sched = sess
        local str = ""
        for i, s in ipairs(sess) do
          if i > 1 then str = str .. ";" end
          str = str .. s.label .. ":" .. s.epoch
        end
        store.set("sched", str)
        fetched = true
        draw()
        print("f1 next race: " .. r.raceName)
        fetch_standings()
        return
      end
    end
    print("f1 race fetch failed: " .. tostring(body):sub(1, 60))
    if not fetched then ui.after(6000, fetch_race) end
  end)
end
ui.after(1200, fetch_race)

-- the car laps the circuit: one lap every 4.5 seconds. The
-- same tick runs the start-lights sequence: if the app is
-- open in the two minutes after lights out (and has not
-- played it for this race), the five lights run once.
ui.every(50, function()
  if lights > 0 then
    lights = lights + 1
    if lights == 49 then sound.effect("powerup", {volume = 50}) end
    if lights > 60 then lights = 0 end
    draw()
    return
  end
  if race and store.get("lights") ~= race.epoch then
    local now = time.now()
    if now >= race.epoch and now <= race.epoch + 120 then
      store.set("lights", race.epoch)
      screen = 1
      lights = 1
      draw()
      return
    end
  end
  if screen == 1 and path then
    anim_dist = anim_dist + path.total * 50 / 4500
    draw()
  end
end)
ui.every(60000, draw)
