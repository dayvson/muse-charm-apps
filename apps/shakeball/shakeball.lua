-- Shake Ball: a magic 8-ball for the round screen.
-- Ask a question out loud, then shake the gadget (or
-- tap it): the ball rattles for a moment and the
-- answer window fills with the classic blue triangle
-- and a verdict, one of the twenty traditional
-- answers. The motion sensor does the listening;
-- a shake is a hard swing of the acceleration, and
-- a tap works too for the gentle. The number of
-- shakes is kept in the store.
--
-- Controls: shake the gadget, or tap the ball.
--
-- Rendering follows the house pattern for games: one
-- 400x400 canvas at zoom 1 in partial mode. The scene
-- is painted on state changes only; while the ball
-- rattles, each frame covers its old spot and draws
-- it at a jittered one.

local BG = 0x17142E
local BALL = 0x0C0C12
local RIM = 0x34344A
local SHEEN = 0x262633
local DISC = 0xF5F5F0
local INK = 0x14141C
local WIN = 0x16235C
local TRI = 0x2E4BD7
local TXT = 0xFFFFFF
local HINT = 0xB9B7D9
local DIM = 0x8E8CA8

local ANSWERS = {
  "IT IS CERTAIN", "IT IS DECIDEDLY SO",
  "WITHOUT A DOUBT", "YES, DEFINITELY",
  "YOU MAY RELY ON IT", "AS I SEE IT, YES",
  "MOST LIKELY", "OUTLOOK GOOD", "YES",
  "SIGNS POINT TO YES",
  "REPLY HAZY, ASK AGAIN", "ASK AGAIN LATER",
  "BETTER NOT TELL YOU NOW", "CANNOT PREDICT NOW",
  "CONCENTRATE, ASK AGAIN",
  "DO NOT COUNT ON IT", "MY REPLY IS NO",
  "MY SOURCES SAY NO", "OUTLOOK NOT SO GOOD",
  "VERY DOUBTFUL",
}

local shakes = store.get("shakes") or 0

local state = "idle"   -- idle | shaking | revealed
local answer = nil
local shake_t = 0
local calm_t = 99      -- time since the last shake
local t_now = 0
local obx, oby = nil, nil   -- ball's last drawn centre

local c = nil   -- set below, everything draws on it

local BCX, BCY, BR = 200, 188, 118

local function wrap(s, per)
  local lines, cur = {}, ""
  for w in s:gmatch("%S+") do
    if #cur == 0 then
      cur = w
    elseif #cur + 1 + #w <= per then
      cur = cur .. " " .. w
    else
      lines[#lines + 1] = cur
      cur = w
    end
  end
  if #cur > 0 then lines[#lines + 1] = cur end
  return lines
end

local function draw_ball(x, y)
  c:circle(x, y, BR + 3, RIM, { fill = true })
  c:circle(x, y, BR, BALL, { fill = true })
  c:circle(x - 40, y - 46, 26, SHEEN, { fill = true })
  -- the white 8 disc
  c:circle(x, y - 58, 33, DISC, { fill = true })
  c:text("8", x, y - 83, INK, 48, "center")
  -- the answer window
  c:circle(x, y + 56, 47, RIM, { fill = true })
  c:circle(x, y + 56, 43, WIN, { fill = true })
  if state == "revealed" and answer then
    c:polygon({ x - 34, y + 82, x + 34, y + 82,
                x, y + 26 }, TRI, { fill = true })
    local lines = wrap(answer, 9)
    local ly = y + 56 - (#lines * 20) / 2 + 3
    for i, ln in ipairs(lines) do
      c:text(ln, x, ly + (i - 1) * 20, TXT, 16, "center")
    end
  elseif state == "idle" then
    c:text("?", x, y + 44, DIM, 28, "center")
  end
end

local function draw_scene()
  c:rect(0, 0, 400, 400, BG, { fill = true })
  draw_ball(BCX, BCY)
  obx, oby = BCX, BCY
  if state == "idle" then
    c:text("ASK, THEN SHAKE", 200, 336, HINT, 16, "center")
  elseif state == "revealed" then
    c:text("SHAKE AGAIN, OR TAP", 200, 336, HINT, 16, "center")
  end
  if shakes > 0 then
    c:text(shakes .. (shakes == 1 and " SHAKE" or " SHAKES"),
           200, 366, DIM, 14, "center")
  end
end

local function start_shake()
  state = "shaking"
  shake_t = 0.75
  calm_t = 0
  sound.effect("tick", { volume = 35 })
end

local function finish_shake()
  local n
  repeat
    n = math.random(#ANSWERS)
  until ANSWERS[n] ~= answer or #ANSWERS < 2
  answer = ANSWERS[n]
  state = "revealed"
  shakes = shakes + 1
  store.set("shakes", shakes)
  sound.effect("select", { volume = 45 })
  draw_scene()
end

local function draw_shake_frame()
  if obx then
    c:rect(obx - BR - 6, oby - BR - 6, 2 * BR + 12,
           2 * BR + 12, BG, { fill = true })
  end
  local jx = math.random(-11, 11)
  local jy = math.random(-9, 9)
  draw_ball(BCX + jx, BCY + jy)
  obx, oby = BCX + jx, BCY + jy
end

-- ---------- input ----------
local function handle_touch(kind, x, y)
  if kind == "down" and state ~= "shaking" then
    start_shake()
  end
end

-- launcher icon: the ball, 8 up
do
  local ic = ui.canvas{ w = 96, h = 96, color = BG }
  ic:circle(48, 48, 42, BALL, { fill = true })
  ic:circle(48, 33, 16, DISC, { fill = true })
  ic:text("8", 48, 21, INK, 28, "center")
  ic:circle(48, 68, 13, WIN, { fill = true })
  app.icon(ic)
  ic:delete()
end

ui.background(BG)

c = ui.canvas{
  w = 400, h = 400, color = BG,
  on_touch = function(kind, x, y) handle_touch(kind, x, y) end,
}
c:partial(true)

draw_scene()

local last_t = time.now()
ui.every(33, function()
  local now = time.now()
  local dt = now - last_t
  last_t = now
  if dt > 0.08 then dt = 0.08 end
  t_now = t_now + dt
  calm_t = calm_t + dt
  if state == "shaking" then
    shake_t = shake_t - dt
    if shake_t <= 0 then
      finish_shake()
    else
      draw_shake_frame()
    end
  elseif calm_t > 0.8 then
    local m = motion.read()
    if m then
      local ax, ay, az = m.ax or 0, m.ay or 0, m.az or 0
      local mag = math.sqrt(ax * ax + ay * ay + az * az)
      if mag > 2.05 then start_shake() end
    end
  end
end)

print("shakeball ready, " .. shakes .. " shakes so far")
