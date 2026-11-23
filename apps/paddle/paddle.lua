-- Paddle Ball: an original brick-breaking game for the round screen.
-- Drag to move the paddle, tap to launch the ball, clear every brick.
-- Partial-frame canvas drawing; sound effects on bounces and breaks.

local BG = 0x101014
local WALL = 0x2C2C34
local PALE = 0xD8D5CC
local LAV = 0x8E8CA8
local GOLD = 0xFFD60A
local ROW_COLORS = {0xC96F4A, 0xD9A441, 0x7D9B6A, 0x5B8C8A, 0x8E8CA8}
local ROW_POINTS = {50, 40, 30, 20, 10}

local W, H = 400, 400
local LEFT, RIGHT, TOP = 16, 384, 38
local BRICK_X, BRICK_Y = 20, 52
local BRICK_W, BRICK_H = 48, 16
local STEP_X, STEP_Y = 52, 21
local COLS, ROWS = 7, 5
local PADDLE_W, PADDLE_H, PADDLE_Y = 76, 12, 352
local BALL_R = 7

local score = 0
local best = store.get("best") or 0
local lives = 3
local level = 1
local state = "ready" -- ready, play, over
local paddle_x = 200
local target_x = 200
local bx, by, vx, vy = 200, 0, 0, 0
local speed = 5.0
local bricks = {}
-- frame-painting state for partial updates: what the last
-- frame actually drew, so the next one repaints only changes
local pending_brick = nil
local need_full = true
local drawn_state = ""

local function build_bricks()
  bricks = {}
  need_full = true
  for r = 1, ROWS do
    for c = 1, COLS do
      bricks[#bricks + 1] = {
        x = BRICK_X + (c - 1) * STEP_X, y = BRICK_Y + (r - 1) * STEP_Y,
        color = ROW_COLORS[r], points = ROW_POINTS[r], alive = true,
      }
    end
  end
end

local function bricks_left()
  local n = 0
  for _, b in ipairs(bricks) do if b.alive then n = n + 1 end end
  return n
end

local function reset_ball()
  state = "ready"
  bx, by = paddle_x, PADDLE_Y - BALL_R - 1
  vx, vy = 0, 0
end

local function new_game()
  score = 0
  lives = 3
  level = 1
  speed = 5.0
  build_bricks()
  reset_ball()
end

local function launch()
  if state ~= "ready" then return end
  state = "play"
  local dir = (math.random(0, 1) == 0) and -1 or 1
  vx = dir * speed * 0.45
  vy = -math.sqrt(speed * speed - vx * vx)
  sound.effect("jump", {volume = 40})
end

local function lose_ball()
  lives = lives - 1
  if lives <= 0 then
    state = "over"
    if score > best then
      best = score
      store.set("best", best)
    end
    sound.effect("lose", {volume = 60})
  else
    sound.effect("explosion", {volume = 55})
    reset_ball()
  end
end

local function clear_level()
  if score > best then
    best = score
    store.set("best", best)
  end
  sound.effect("win", {volume = 60})
  level = level + 1
  speed = math.min(5.0 + (level - 1) * 0.5, 8.5)
  build_bricks()
  reset_ball()
end

-- launcher icon: bricks, a ball and the paddle
do
  local ic = ui.canvas{w = 96, h = 96, color = 0x101014}
  ic:rect(14, 14, 20, 10, 0xC96F4A, {fill = true, radius = 3})
  ic:rect(38, 14, 20, 10, 0xD9A441, {fill = true, radius = 3})
  ic:rect(62, 14, 20, 10, 0x7D9B6A, {fill = true, radius = 3})
  ic:circle(48, 42, 9, 0xFFD60A, {fill = true})
  ic:rect(24, 68, 48, 11, 0xD8D5CC, {fill = true, radius = 5})
  app.icon(ic)
  ic:delete()
end

ui.background(BG) -- the playfield colour fills the whole round screen

local function handle_touch(kind, x, y)
  if kind == "down" or kind == "move" then
    target_x = x
    if kind == "down" then
      if state == "over" then new_game() else launch() end
    end
  end
end

-- the drawing surface is a 400x400 canvas at zoom 1: the
-- perf readout showed a zoomed canvas costs about 36 ms
-- extra per frame in the compositor (frame_ms 81 at zoom 2
-- against 45 native, same scene), so the game draws in its
-- own 400-wide space with no scaling proxy; touches arrive
-- in the same coordinates
local raw = ui.canvas{
  w = 400, h = 400, color = BG,
  on_touch = function(kind, x, y)
    handle_touch(kind, x, y)
  end,
}
-- partial frames: only the areas painted since the last clear
-- (plus the previous frame's) go to the screen; the moving
-- parts here are small on a plain background, so frames cost
-- a fraction of a full canvas send
raw:partial(true)
local c = raw

local function step_ball(k)
  local prev_x, prev_y = bx, by
  bx = bx + vx * k
  by = by + vy * k

  if bx - BALL_R < LEFT then
    bx = LEFT + BALL_R; vx = math.abs(vx)
    sound.effect("tick", {volume = 18})
  elseif bx + BALL_R > RIGHT then
    bx = RIGHT - BALL_R; vx = -math.abs(vx)
    sound.effect("tick", {volume = 18})
  end
  if by - BALL_R < TOP then
    by = TOP + BALL_R; vy = math.abs(vy)
    sound.effect("tick", {volume = 18})
  end

  -- paddle
  if vy > 0 and prev_y + BALL_R <= PADDLE_Y + 1
     and by + BALL_R >= PADDLE_Y
     and math.abs(bx - paddle_x) <= PADDLE_W / 2 + BALL_R - 2 then
    local off = (bx - paddle_x) / (PADDLE_W / 2)
    if off > 1 then off = 1 elseif off < -1 then off = -1 end
    vx = off * speed * 0.85
    vy = -math.sqrt(math.max(speed * speed - vx * vx, 1))
    by = PADDLE_Y - BALL_R
    sound.effect("hit", {volume = 60})
    return
  end

  -- bricks
  for _, b in ipairs(bricks) do
    if b.alive
       and bx + BALL_R > b.x and bx - BALL_R < b.x + BRICK_W
       and by + BALL_R > b.y and by - BALL_R < b.y + BRICK_H then
      b.alive = false
      pending_brick = b
      score = score + b.points
      speed = math.min(speed + 0.06, 8.5)
      if prev_y + BALL_R <= b.y or prev_y - BALL_R >= b.y + BRICK_H then
        vy = -vy
      elseif prev_x + BALL_R <= b.x or prev_x - BALL_R >= b.x + BRICK_W then
        vx = -vx
      else
        vy = -vy
      end
      sound.effect("coin", {volume = 45})
      if bricks_left() == 0 then clear_level() end
      return
    end
  end

  if by - BALL_R > H - 4 then lose_ball() end
end

-- option tables for the shapes drawn every frame, kept at
-- file scope so draw() builds no tables it can avoid, and
-- the HUD strings rebuild only when their value changes
local BRICK_O = {fill = true, radius = 3}
local PADDLE_O = {fill = true, radius = 6}
local BALL_O = {fill = true}
local FILL_O = {fill = true}
local last_score, last_level, last_lives = -1, -1, -1
local hud_score, hud_level, hud_balls = "", "", ""

local function draw_hud()
  c:text(hud_score, 20, 14, PALE, 14, "left")
  c:text(hud_level, 200, 14, LAV, 14, "center")
  c:text(hud_balls, 380, 14, PALE, 14, "right")
end

local function draw_walls()
  c:line(LEFT, TOP, RIGHT, TOP, WALL, 2)
  c:line(LEFT, TOP, LEFT, 392, WALL, 2)
  c:line(RIGHT, TOP, RIGHT, 392, WALL, 2)
end

local function draw_state_text()
  if state == "ready" then
    c:text("TAP TO LAUNCH", 200, 300, LAV, 16, "center")
  elseif state == "over" then
    c:text("GAME OVER", 200, 210, 0xE5484D, 28, "center")
    c:text("SCORE " .. score .. "   BEST " .. best, 200, 252, PALE, 16,
           "center")
    c:text("TAP TO PLAY AGAIN", 200, 286, LAV, 16, "center")
  end
  drawn_state = state
end

-- partial-frame drawing: every frame starts with c:clear(),
-- which resets the firmware's dirty window; only what this
-- frame paints (plus last frame's painted areas, re-sent so
-- movers leave no trail) goes to the screen. So a frame
-- paints the two movers and nothing else; the bricks, walls
-- and texts stay on the panel from the frame that painted
-- them, and a static is repainted only when it changes: a
-- dead brick is covered once, a changed HUD line redrawn
-- once, the state text swapped on transitions. A full
-- repaint happens on the first frame and on new levels.
local function draw()
  local hud_changed = false
  if score ~= last_score then
    last_score = score
    hud_score = "SCORE " .. score
    hud_changed = true
  end
  if level ~= last_level then
    last_level = level
    hud_level = "LV " .. level
    hud_changed = true
  end
  if lives ~= last_lives then
    last_lives = lives
    hud_balls = "BALLS " .. lives
    hud_changed = true
  end

  c:clear(BG)
  if need_full then
    draw_hud()
    draw_walls()
    for _, b in ipairs(bricks) do
      if b.alive then
        raw:rect(b.x, b.y, BRICK_W, BRICK_H, b.color, BRICK_O)
      end
    end
    draw_state_text()
    need_full = false
    pending_brick = nil
  else
    if pending_brick then
      raw:rect(pending_brick.x, pending_brick.y,
               BRICK_W, BRICK_H, BG, FILL_O)
      pending_brick = nil
    end
    if hud_changed then
      raw:rect(0, 8, 400, 26, BG, FILL_O)
      draw_hud()
    end
    if state ~= drawn_state then
      raw:rect(50, 196, 300, 116, BG, FILL_O)
      draw_state_text()
    end
  end
  raw:rect(paddle_x - PADDLE_W / 2, PADDLE_Y,
           PADDLE_W, PADDLE_H, PALE, PADDLE_O)
  raw:circle(bx, by, BALL_R, GOLD, BALL_O)
end

-- the frame rate is set by the screen flush, not the timer,
-- so physics runs on elapsed time: the ball advances as far
-- per second at 17 fps as it did in the 30 fps design, in
-- two substeps so it cannot tunnel through a brick
local last_t = 0
ui.every(20, function()
  local now = time.now()
  local dt = now - last_t
  last_t = now
  if dt <= 0 or dt > 0.25 then dt = 0.02 end
  local f = dt / 0.0333
  if f < 0.3 then f = 0.3 elseif f > 3 then f = 3 end

  paddle_x = paddle_x + (target_x - paddle_x)
             * (1 - 0.45 ^ (dt / 0.02))
  local half = PADDLE_W / 2
  if paddle_x < LEFT + half then paddle_x = LEFT + half end
  if paddle_x > RIGHT - half then paddle_x = RIGHT - half end

  if state == "ready" then
    bx, by = paddle_x, PADDLE_Y - BALL_R - 1
  elseif state == "play" then
    step_ball(f / 2)
    if state == "play" then step_ball(f / 2) end
  end
  draw()
end)

build_bricks()
reset_ball()
last_t = time.now()
print("paddle ready, best " .. best)
local gfx_used, gfx_avail = gfx.memory()
print("paddle gfx memory: used " .. tostring(gfx_used) ..
      ", available " .. tostring(gfx_avail))
