-- Lab: a test bench for the new firmware APIs. A roller at
-- the top picks the page; the bottom button cycles pages too.
-- Device: device.info() readouts and a live mic.level() bar.
-- Motion: motion.read() accelerometer and gyroscope values.
-- Ask: a button that sends muse.ask and shows the reply.
-- Plus a 1-minute app.alarm test and an app.on_message state
-- reply for luapp.send.

local BG = 0x0B0F14
local PANEL = 0x1C1C1E
local TEAL = 0x27D3A2
local PALE = 0xE8E6DF
local LAV = 0x8E8CA8
local GOLD = 0xD9A441
local RED = 0xE5484D

local NAMES = {"Device", "Motion", "Ask Muse"}
local page = 1
local last_motion = nil

ui.background(BG)

-- launcher icon: an atom, a teal ring and a gold core
do
  local ic = ui.canvas{w = 96, h = 96, color = 0x14532D}
  ic:circle(48, 48, 30, TEAL, {fill = false, width = 5})
  ic:circle(48, 48, 10, GOLD, {fill = true})
  ic:circle(48, 18, 6, PALE, {fill = true})
  app.icon(ic)
  ic:delete()
end

ui.label{text = "LAB", y = -196, size = 16, color = LAV}

local pages = {{}, {}, {}}
local function reg(p, w)
  pages[p][#pages[p] + 1] = w
  return w
end

-- page 1: device
local d_batt = reg(1, ui.label{text = "Battery ?", y = -36, size = 20})
local d_net = reg(1, ui.label{text = "", y = -4, size = 16, color = LAV})
local d_sys = reg(1, ui.label{text = "", y = 24, size = 16, color = LAV})
local d_mic = reg(1, ui.label{text = "MIC", y = 62, size = 16, color = LAV})
local d_bar = reg(1, ui.bar{y = 90, w = 240, h = 12, min = 0, max = 100,
                            value = 0, color = TEAL, bg = PANEL})
local d_alarm = reg(1, ui.label{text = "", y = 122, size = 14,
                                color = GOLD})
reg(1, ui.button{text = "ALARM IN 1 MIN", y = 162, w = 220, h = 46,
  color = PANEL, text_color = TEAL, size = 16,
  on_tap = function()
    app.alarm(60, "lab")
    local t = time.date(time.now() + 60)
    d_alarm:set{text = string.format("Alarm set for %02d:%02d",
                                     t.hour, t.min)}
    print("lab alarm set for 60 s")
  end})

-- page 2: motion
local m_ax = reg(2, ui.label{text = "AX ?", y = -52, size = 20})
local m_ay = reg(2, ui.label{text = "AY ?", y = -20, size = 20})
local m_az = reg(2, ui.label{text = "AZ ?", y = 12, size = 20})
local m_gx = reg(2, ui.label{text = "GX ?", y = 56, size = 16,
                             color = LAV})
local m_gy = reg(2, ui.label{text = "GY ?", y = 84, size = 16,
                             color = LAV})
local m_gz = reg(2, ui.label{text = "GZ ?", y = 112, size = 16,
                             color = LAV})
reg(2, ui.label{text = "g and deg/s, live", y = 158, size = 14,
                color = LAV})

-- page 3: ask Muse
local a_ans = reg(3, ui.label{text = "", y = 36, size = 20, w = 300})
local a_stat = reg(3, ui.label{text = "", y = 118, size = 14,
                               color = LAV})
reg(3, ui.button{text = "ASK CLAUDE", y = -36, w = 220, h = 54,
  color = TEAL, text_color = 0x06281C, size = 20,
  on_tap = function()
    a_stat:set{text = "Asking..."}
    muse.ask("Say hi in five words", function(answer, err)
      if answer then
        a_ans:set{text = answer}
        a_stat:set{text = ""}
        print("lab ask answered: " .. answer)
      else
        a_stat:set{text = "Error: " .. tostring(err)}
        print("lab ask failed: " .. tostring(err))
      end
    end)
  end})

local function show_page(p)
  page = p
  for i = 1, 3 do
    for _, w in ipairs(pages[i]) do
      if i == p then w:show() else w:hide() end
    end
  end
end

local roller = ui.roller{
  options = NAMES, value = 1, rows = 3, y = -132, w = 250, size = 20,
  bg = PANEL, color = TEAL, text_color = LAV,
  on_change = function(i) show_page(i) end,
}

-- the bottom button cycles pages and moves the roller along
ui.on_button(function()
  local p = page % 3 + 1
  roller:set{value = p}
  show_page(p)
  sound.effect("click", {volume = 25})
end)

-- the alarm test: fires even if the app was closed; the gadget
-- wakes, opens Lab fresh and this runs
app.on_alarm(function(tag)
  print("lab alarm fired: " .. tostring(tag))
  d_alarm:set{text = "ALARM FIRED"}
  sound.effect("win", {volume = 55})
end)

-- Muse can ask the running app for its state with luapp.send;
-- motion is read on demand so it answers from any page
app.on_message(function(data)
  local info = device.info()
  local ax, ay, az, gx, gy, gz = motion.read()
  local m = nil
  if ax then
    m = {ax = ax, ay = ay, az = az, gx = gx, gy = gy, gz = gz}
    last_motion = m
  end
  return {
    page = NAMES[page],
    battery = info.battery,
    muse = info.muse,
    motion = m,
  }
end)

local function fmt(v)
  return string.format("%.2f", v)
end

local function update_info()
  local info = device.info()
  local b = info.battery
  d_batt:set{text = "Battery " ..
    (b and (b .. "%" .. (info.charging and " +chg" or "")) or "?")}
  d_net:set{text = "Wi-Fi " .. (info.wifi and "on" or "off") ..
    " - Muse " .. tostring(info.muse)}
  d_sys:set{text = "Vol " .. tostring(info.volume) ..
    " - Up " .. math.floor((info.uptime or 0) / 60) .. "m"}
end

ui.every(250, function()
  if page == 1 then
    local db = mic.level()
    local v = math.floor((db + 60) / 60 * 100)
    if v < 0 then v = 0 elseif v > 100 then v = 100 end
    d_bar:set{value = v}
    d_mic:set{text = "MIC " .. math.floor(db) .. " dB"}
  elseif page == 2 then
    local ax, ay, az, gx, gy, gz = motion.read()
    if ax then
      last_motion = {ax = ax, ay = ay, az = az,
                     gx = gx, gy = gy, gz = gz}
      m_ax:set{text = "AX " .. fmt(ax)}
      m_ay:set{text = "AY " .. fmt(ay)}
      m_az:set{text = "AZ " .. fmt(az)}
      m_gx:set{text = "GX " .. fmt(gx)}
      m_gy:set{text = "GY " .. fmt(gy)}
      m_gz:set{text = "GZ " .. fmt(gz)}
    else
      m_ax:set{text = "Motion: " .. tostring(ay)}
    end
  end
end)

ui.every(1000, function()
  if page == 1 then update_info() end
end)

show_page(1)
update_info()
print("lab ready")
