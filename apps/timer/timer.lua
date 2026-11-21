-- Timer: one timer in the middle, starting at 5 minutes, with a
-- minus button on the left and a plus button on the right to set
-- the duration (1 minute steps, 1 to 60 minutes). Tap the screen
-- to start, tap again to pause. A soft tick sounds each second
-- while it runs. A ring around the rim is eaten away as time
-- passes: green, yellow in the last minute (or the last 35% of a
-- short timer), red for the last stretch. At zero the screen
-- blinks red and a five second tune loops until the timer is
-- stopped with a tap.
-- Runs on the gadget itself (Apps by Muse launcher, Lua).

local GREEN = 0x34C759
local YELLOW = 0xF2C230
local RED = 0xE5484D
local FLASH = 0xD7263D
local BG = 0x121214
local TRACK = 0x23232B
local PALE = 0xD8D5CC
local LAV = 0x8E8CA8
local BTN = 0x2C2C2E

local total = store.get("total") or 300
local left = total
local running = false
local done = false
local deadline = 0
local last_sec = -1
local last_permille = -1
local last_color = nil
local flash_id = nil
local alarm_id = nil

-- launcher icon: a clock on the app's green
do
  local ic = ui.canvas{w = 96, h = 96, color = 0x34C759}
  ic:circle(48, 48, 29, 0xF5F5F0, {fill = false, width = 6})
  ic:line(48, 48, 48, 31, 0xF5F5F0, 6)
  ic:line(48, 48, 61, 55, 0xF5F5F0, 6)
  ic:circle(48, 48, 4, 0xF5F5F0, {fill = true})
  app.icon(ic)
  ic:delete()
end

ui.background(BG)

local ring = ui.arc{r = 214, width = 16, max = 1000, value = 1000,
                    color = GREEN, bg = TRACK}
local time_label = ui.label{text = "5:00", y = -12, size = 48, color = PALE}
local state_label = ui.label{text = "TAP TO START", y = -72, size = 20,
                             color = LAV}

local function fmt(sec)
  if sec < 0 then sec = 0 end
  return string.format("%d:%02d", math.floor(sec / 60),
                       math.floor(sec % 60))
end

local function phase(sec)
  if sec <= 15 or sec * 100 <= total * 15 then return RED end
  if sec <= 60 or sec * 100 <= total * 35 then return YELLOW end
  return GREEN
end

local function paint()
  local permille = math.floor(left * 1000 / total)
  if permille ~= last_permille then
    last_permille = permille
    ring:set{value = permille}
  end
  local c = done and RED or phase(left)
  if c ~= last_color then
    last_color = c
    ring:set{color = c}
  end
  local sec = math.ceil(left)
  if sec ~= last_sec then
    last_sec = sec
    time_label:set{text = fmt(sec)}
    if running and not done then
      sound.effect("tick", {volume = 14})
    end
  end
end

local function stop_alerts()
  if flash_id then ui.cancel(flash_id) flash_id = nil end
  if alarm_id then ui.cancel(alarm_id) alarm_id = nil end
  sound.stop()
  ui.background(BG)
end

-- an original five second tune; it loops until the timer stops
local function alarm()
  sound.play("E5:4 G5:4 C6:4 G5:4 A5:4 G5:4 E5:4 D5:4",
             {bpm = 100, volume = 55})
end

local function finish()
  running = false
  done = true
  left = 0
  paint()
  ring:set{value = 1000, color = RED}
  time_label:set{text = "TIME UP", color = RED}
  state_label:set{text = "TAP TO STOP"}
  alarm()
  alarm_id = ui.every(5000, alarm)
  local lit = false
  flash_id = ui.every(450, function()
    lit = not lit
    if lit then
      ui.background(FLASH)
      time_label:set{color = PALE}
    else
      ui.background(BG)
      time_label:set{color = RED}
    end
  end)
end

local function reset()
  stop_alerts()
  done = false
  running = false
  left = total
  last_sec = -1
  last_permille = -1
  last_color = nil
  time_label:set{color = PALE}
  state_label:set{text = "TAP TO START"}
  paint()
end

local function toggle()
  if done then
    reset()
    return
  end
  if running then
    left = deadline - time.now()
    running = false
    state_label:set{text = "PAUSED"}
    paint()
  else
    deadline = time.now() + left
    running = true
    state_label:set{text = "RUNNING"}
  end
end

local function adjust(dir)
  if done then return end
  local t = total + dir * 60
  if t < 60 then t = 60 end
  if t > 3600 then t = 3600 end
  if t == total then return end
  total = t
  store.set("total", total)
  sound.effect("select", {volume = 25})
  reset()
end

ui.button{
  text = "-", x = -152, y = -12, w = 64, h = 64, size = 28,
  color = BTN, text_color = PALE, radius = 32,
  on_tap = function() adjust(-1) end,
}
ui.button{
  text = "+", x = 152, y = -12, w = 64, h = 64, size = 28,
  color = BTN, text_color = PALE, radius = 32,
  on_tap = function() adjust(1) end,
}

ui.on_tap(function(x, y) toggle() end)

ui.every(100, function()
  if running then
    left = deadline - time.now()
    if left <= 0 then
      finish()
    else
      paint()
    end
  end
end)

reset()
print("timer ready, set " .. fmt(total))
