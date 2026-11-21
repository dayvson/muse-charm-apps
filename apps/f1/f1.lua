-- Grand Prix: the next Formula 1 race and the championship top 3.
-- Screen one shows the next race under a checkered band: its
-- name, a big drawn outline of the circuit with a red dot
-- lapping it like a car, and the start time in local time with
-- a countdown. Screen two shows the top 3 drivers, each with
-- their flag and their team's colour. Swipe or tap to switch.
-- Data comes from the free, keyless Ergast mirror at
-- api.jolpi.ca and is cached in the store, so the app opens
-- instantly and works offline with the last saved data.

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

local race = nil   -- {name, cid, circuit, loc, country, epoch}
local top = {}     -- up to 3 {code, name, team, pts, wins, nat}
local season, round = nil, nil
local state = "offline" -- live, saved, offline
local screen = 1
local path = nil   -- {pts, cum, total, cid} for the lap animation
local anim_dist = 0

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

local function countdown(epoch)
  local diff = epoch - time.now()
  if diff <= 0 then return "race weekend is on" end
  local days = math.floor(diff / 86400)
  local hrs = math.floor((diff % 86400) / 3600)
  local min = math.floor((diff % 3600) / 60)
  if days > 0 then return "in " .. days .. "d " .. hrs .. "h" end
  if hrs > 0 then return "in " .. hrs .. "h " .. min .. "m" end
  return "in " .. min .. "m"
end

local c

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

local function draw_band()
  for row = 0, 1 do
    for i = 0, 19 do
      local col = ((i + row) % 2 == 0) and PALE or 0x17171F
      c:rect(66 + i * 11, 6 + row * 11, 11, 11, col)
    end
  end
end

local function build_path(cid)
  local raw = OUTLINES[cid]
  if not raw then path = nil return end
  local cx, cy, half = 176, 180, 84
  local pts, cum, total = {}, {}, 0
  for i, p in ipairs(raw) do
    pts[i] = {cx + (p[1] - 50) / 50 * half,
              cy + (p[2] - 50) / 50 * half}
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
    c:circle(176, 168, 30, GOLD, {fill = false, width = 4})
    c:circle(176, 168, 10, GOLD, {fill = true})
    c:triangle(158, 186, 194, 186, 176, 222, GOLD)
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

local function draw_race()
  c:text("NEXT RACE", 176, 34, LAV, 13, "center")
  if not race then
    c:text("No race data yet", 176, 150, PALE, 20, "center")
    c:text("check your connection", 176, 182, LAV, 14, "center")
    return
  end
  local head = race.name:gsub(" Grand Prix$", "")
  c:text(head, 176, 52, PALE, 30, "center")
  c:text("GRAND PRIX", 176, 86, GOLD, 15, "center")
  draw_outline(race.cid)
  c:text(race_local_parts(race.epoch), 176, 282, PALE, 20, "center")
  c:text(countdown(race.epoch) .. "  -  " .. state, 176, 308,
         GOLD, 14, "center")
end

local function draw_standings()
  c:text("CHAMPIONSHIP", 176, 36, LAV, 13, "center")
  if season then
    c:text(season .. " - after round " .. round, 176, 56, GOLD, 14,
           "center")
  end
  if #top == 0 then
    c:text("No standings yet", 176, 150, PALE, 20, "center")
    return
  end
  for i, d in ipairs(top) do
    local y = 92 + (i - 1) * 62
    c:rect(22, y, 5, 42, team_color(d.team))
    draw_flag(36, y + 4, d.nat)
    c:text(i .. ". " .. d.code .. " - " .. d.name, 86, y + 2,
           i == 1 and GOLD or PALE, 19, "left")
    c:text(d.team .. " - " .. d.pts .. " pts - " .. d.wins ..
           (d.wins == 1 and " win" or " wins"), 86, y + 28,
           LAV, 13, "left")
    if i < #top then c:line(22, y + 52, 330, y + 52, DIM, 1) end
  end
  c:text("top 3 in the title race", 176, 300, LAV, 14, "center")
end

local function draw()
  c:clear(BG)
  draw_band()
  if screen == 1 then draw_race() else draw_standings() end
  for i = 1, 2 do
    c:circle(166 + (i - 1) * 20, 338, 4,
             i == screen and PALE or DIM, {fill = true})
  end
end

-- launcher icon: a white circuit loop on F1 red
do
  local ic = ui.canvas{w = 96, h = 96, color = RED}
  local pts = {{24,38},{40,24},{62,22},{76,34},{70,50},{78,64},
               {60,74},{40,70},{26,58}}
  for i = 1, #pts do
    local a = pts[i]
    local b = pts[i % #pts + 1]
    ic:line(a[1], a[2], b[1], b[2], 0xFFFFFF, 5)
  end
  ic:circle(24, 38, 5, 0xFFFFFF, {fill = true})
  app.icon(ic)
  ic:delete()
end

ui.background(BG)

local down_x = nil
c = ui.canvas{
  w = 352, h = 352, color = BG,
  on_touch = function(kind, x, y)
    if kind == "down" then
      down_x = x
    elseif kind == "up" and down_x then
      local dx = x - down_x
      down_x = nil
      if dx < -40 then screen = 2
      elseif dx > 40 then screen = 1
      elseif math.abs(dx) <= 12 then screen = (screen == 1) and 2 or 1
      else return end
      sound.effect("select", {volume = 25})
      draw()
    end
  end,
}

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
        return
      end
    end
    print("f1 standings fetch failed: " .. tostring(body):sub(1, 60))
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
        local y, mo, d = r.date:match("^(%d+)-(%d+)-(%d+)$")
        local hh, mi = (r.time or "00:00:00Z"):match("^(%d+):(%d+)")
        save_race({
          name = r.raceName, cid = r.Circuit.circuitId,
          circuit = r.Circuit.circuitName,
          loc = r.Circuit.Location.locality,
          country = r.Circuit.Location.country,
          epoch = days_from_civil(tonumber(y), tonumber(mo),
                                  tonumber(d)) * 86400
                  + tonumber(hh) * 3600 + tonumber(mi) * 60,
        })
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

-- the car laps the circuit: one lap every 4.5 seconds
ui.every(50, function()
  if screen == 1 and path then
    anim_dist = anim_dist + path.total * 50 / 4500
    draw()
  end
end)
ui.every(60000, draw)
