-- Grand Prix: the next Formula 1 race and the championship top 3.
-- Screen one shows the next race: its name, a drawn outline of
-- the circuit, and the start time in local time with a countdown.
-- Screen two shows the top 3 drivers with points and wins.
-- Swipe or tap to switch screens. Data comes from the free,
-- keyless Ergast mirror at api.jolpi.ca and is cached in the
-- store, so the app opens instantly and works offline with the
-- last saved data.

local BG = 0x0B0B10
local PALE = 0xD8D5CC
local LAV = 0x8E8CA8
local GOLD = 0xD9A441
local RED = 0xE10600
local DIM = 0x3A3A48

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

local race = nil   -- {name, cid, circuit, loc, country, epoch}
local top = {}     -- up to 3 {code, name, team, pts, wins}
local season, round = nil, nil
local state = "offline" -- live, saved, offline
local screen = 1

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
      local code, name, team, pts, wins =
        s:match("^(.-)|(.-)|(.-)|(.-)|(.+)$")
      if code then
        top[i] = {code = code, name = name, team = team,
                  pts = tonumber(pts), wins = tonumber(wins)}
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

local function draw_outline(cid)
  local pts = OUTLINES[cid]
  local cx, cy, half = 176, 162, 60
  if not pts then
    -- no tracing for this circuit yet: a location pin instead
    c:circle(cx, cy - 12, 24, GOLD, {fill = false, width = 4})
    c:circle(cx, cy - 12, 8, GOLD, {fill = true})
    c:triangle(cx - 16, cy + 2, cx + 16, cy + 2, cx, cy + 30, GOLD)
    return
  end
  local function px(p) return cx + (p[1] - 50) / 50 * half end
  local function py(p) return cy + (p[2] - 50) / 50 * half end
  for i = 1, #pts do
    local a = pts[i]
    local b = pts[i % #pts + 1]
    c:line(px(a), py(a), px(b), py(b), PALE, 3)
  end
  c:circle(px(pts[1]), py(pts[1]), 5, RED, {fill = true})
end

local function draw_race()
  c:text("NEXT RACE", 176, 16, LAV, 14, "center")
  if not race then
    c:text("No race data yet", 176, 150, PALE, 20, "center")
    c:text("check your connection", 176, 182, LAV, 14, "center")
    return
  end
  local head = race.name:gsub(" Grand Prix$", "")
  c:text(head, 176, 36, PALE, 28, "center")
  c:text("GRAND PRIX", 176, 70, GOLD, 16, "center")
  draw_outline(race.cid)
  c:text(race.circuit, 176, 240, PALE, 14, "center")
  c:text(race.loc .. ", " .. race.country, 176, 260, LAV, 14, "center")
  c:text(race_local_parts(race.epoch), 176, 288, PALE, 20, "center")
  c:text(countdown(race.epoch) .. "  -  " .. state, 176, 316,
         GOLD, 14, "center")
end

local function draw_standings()
  c:text("CHAMPIONSHIP", 176, 16, LAV, 14, "center")
  if season then
    c:text(season .. " - after round " .. round, 176, 38, GOLD, 14,
           "center")
  end
  if #top == 0 then
    c:text("No standings yet", 176, 150, PALE, 20, "center")
    return
  end
  for i, d in ipairs(top) do
    local y = 76 + (i - 1) * 62
    c:text(i .. ". " .. d.code .. " - " .. d.name, 52, y,
           i == 1 and GOLD or PALE, 20, "left")
    c:text(d.team .. " - " .. d.pts .. " pts - " .. d.wins ..
           (d.wins == 1 and " win" or " wins"), 52, y + 26,
           LAV, 14, "left")
  end
  c:text("top 3 in the title race", 176, 292, LAV, 14, "center")
end

local function draw()
  c:clear(BG)
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
                     pts = tonumber(e.points), wins = tonumber(e.wins)}
          top[i] = d
          store.set("top" .. i, d.code .. "|" .. d.name .. "|" ..
                    d.team .. "|" .. d.pts .. "|" .. d.wins)
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

ui.every(60000, draw)
