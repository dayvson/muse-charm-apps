-- Timer: pick 2, 3 or 5 minutes, tap to start, tap again to pause.
-- A ring around the rim is eaten away as time runs: green, yellow in
-- the last minute (or the last 35% of a short timer), red for the
-- last stretch. At zero the whole screen blinks red with TIME UP.
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
local BTN_SEL = 0x44506B

local PRESETS = {120, 180, 300}
local NAMES = {"2m", "3m", "5m"}

local preset = store.get("preset") or 3
local total = PRESETS[preset]
local left = total
local running = false
local done = false
local deadline = 0
local last_sec = -1
local last_permille = -1
local last_color = nil
local flash_ticks = 0
local flash_id = nil

ui.background(BG)

local ring = ui.arc{r = 214, width = 16, max = 1000, value = 1000,
                    color = GREEN, bg = TRACK}
local time_label = ui.label{text = "5:00", y = -12, size = 48, color = PALE}
local state_label = ui.label{text = "TAP TO START", y = -72, size = 20,
                             color = LAV}

local buttons = {}

local function paint_buttons()
  for i, b in ipairs(buttons) do
    b:set{color = (i == preset) and BTN_SEL or BTN}
  end
end

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
  end
end

local function stop_flash()
  if flash_id then ui.cancel(flash_id) flash_id = nil end
  ui.background(BG)
end

local function finish()
  running = false
  done = true
  left = 0
  paint()
  ring:set{value = 1000, color = RED}
  time_label:set{text = "TIME UP", color = RED}
  state_label:set{text = "TAP TO RESET"}
  flash_ticks = 0
  flash_id = ui.every(450, function()
    flash_ticks = flash_ticks + 1
    if flash_ticks % 2 == 1 then
      ui.background(FLASH)
      time_label:set{color = PALE}
    else
      ui.background(BG)
      time_label:set{color = RED}
    end
    if flash_ticks >= 13 then stop_flash() end
  end)
end

local function reset()
  stop_flash()
  done = false
  running = false
  total = PRESETS[preset]
  left = total
  last_sec = -1
  last_permille = -1
  last_color = nil
  time_label:set{color = PALE}
  state_label:set{text = "TAP TO START"}
  paint()
  paint_buttons()
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

for i, name in ipairs(NAMES) do
  buttons[i] = ui.button{
    text = name, x = (i - 2) * 92, y = 108, w = 80, h = 52, size = 20,
    color = BTN, text_color = PALE, radius = 26,
    on_tap = function()
      if done then return end
      preset = i
      store.set("preset", i)
      reset()
    end,
  }
end

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
print("timer ready, preset " .. NAMES[preset])
