-- Invaders: the classic fixed shooter, drawn for the
-- round screen. A formation of invaders marches across
-- the sky, dropping as it goes; you slide the cannon
-- along the bottom and shoot them down before they land.
--
-- Controls: touch and drag to steer the cannon (it runs
-- to your finger), tap or hold to fire. Three lives,
-- faster and lower waves as you clear them, best score
-- kept in the store.
--
-- Rendering follows the house pattern for games: one
-- 400x400 canvas at zoom 1 in partial mode. The scene is
-- painted once, then each frame only covers the movers'
-- old spots and draws them at the new ones.

local BG = 0x05060A
local WHITE = 0xF2F2F2
local GREY = 0x8A8F98
local GREEN = 0x39D353
local YELLOW = 0xFFD60A

local COLS, ROWS_N = 5, 4
local IW, IH = 30, 20
local PITCH_X, PITCH_Y = 62, 40
local FORM_W = (COLS - 1) * PITCH_X + IW
local CANNON_Y = 356
local DANGER_Y = 342

local ROW_STYLE = {
  { color = 0xFF5DA2, pts = 30 },
  { color = 0x4CC9F0, pts = 20 },
  { color = 0x39D353, pts = 10 },
  { color = 0xB8E62E, pts = 10 },
}

local best = store.get("best") or 0

-- ---------- sprites ----------
-- one invader in its 30x20 box; leg picks the leg frame,
-- s scales every rect (the launcher icon uses s = 2)
local function draw_invader(cv, x, y, color, leg, s)
  s = s or 1
  local function r(rx, ry, rw, rh, col)
    cv:rect(x + rx * s, y + ry * s, rw * s, rh * s, col,
            { fill = true })
  end
  r(6, 0, 3, 5, color) r(21, 0, 3, 5, color)     -- feelers
  r(3, 5, 24, 9, color)                          -- body
  r(8, 8, 4, 4, BG) r(18, 8, 4, 4, BG)           -- eyes
  r(13, 11, 4, 3, BG)                            -- mouth
  if leg == 0 then
    r(3, 14, 4, 6, color) r(23, 14, 4, 6, color)
  else
    r(7, 14, 4, 6, color) r(19, 14, 4, 6, color)
  end
  r(13, 14, 4, 3, color)
end

local function draw_cannon(cv, cx)
  cv:rect(cx - 2, CANNON_Y, 4, 6, GREEN, { fill = true })
  cv:rect(cx - 7, CANNON_Y + 3, 14, 4, GREEN, { fill = true })
  cv:rect(cx - 18, CANNON_Y + 6, 36, 8, GREEN,
          { fill = true, radius = 2 })
end

-- ---------- state ----------
local state = "ready"     -- ready | play | boom | over
local score, lives, wave = 0, 3, 1
local inv = {}            -- {col,row,alive,dx,dy,dleg}
local ox, oy, dir, leg = 61, 54, 1, 0
local alive_n = 0
local cannon_x, target_x = 200, 200
local touching = false
local bullets, bombs = {}, {}
local fire_cd, bomb_acc, step_acc = 0, 0, 0
local boom_t, banner_t = 0, 0
local banner_shown = false
local death_x = 200

local drawn_cx = nil
local drawn_score, drawn_wave, drawn_lives = -1, -1, -1

local function setup_wave()
  inv = {}
  for row = 1, ROWS_N do
    for col = 0, COLS - 1 do
      inv[#inv + 1] = { col = col, row = row, alive = true }
    end
  end
  alive_n = COLS * ROWS_N
  ox = 61
  oy = 54 + math.min((wave - 1) * 10, 42)
  dir, leg = 1, 0
  bullets, bombs = {}, {}
  fire_cd, bomb_acc, step_acc = 0, 0, 0
  banner_t = 1.1
end

-- ---------- screens ----------
local c = nil   -- set below, the screens draw on it

local function draw_hud_full()
  c:text("SCORE " .. score, 14, 8, WHITE, 15, "left")
  c:text("WAVE " .. wave, 200, 8, GREY, 15, "center")
  for i = 0, lives - 1 do
    c:rect(382 - i * 22, 11, 16, 8, GREEN, { fill = true })
  end
  drawn_score, drawn_wave, drawn_lives = score, wave, lives
end

local function draw_play_full()
  c:rect(0, 0, 400, 400, BG, { fill = true })
  draw_hud_full()
  for _, v in ipairs(inv) do
    if v.alive then
      local x = ox + v.col * PITCH_X
      local y = oy + (v.row - 1) * PITCH_Y
      draw_invader(c, x, y, ROW_STYLE[v.row].color, leg)
      v.dx, v.dy, v.dleg = x, y, leg
    else
      v.dx = nil
    end
  end
  draw_cannon(c, cannon_x)
  drawn_cx = cannon_x
  if banner_t > 0 then
    c:text("WAVE " .. wave, 200, 188, WHITE, 30, "center")
  end
end

local function draw_ready()
  c:rect(0, 0, 400, 400, BG, { fill = true })
  c:text("INVADERS", 200, 52, WHITE, 38, "center")
  local rows = { 1, 2, 3 }
  for i, row in ipairs(rows) do
    local y = 118 + (i - 1) * 44
    draw_invader(c, 118, y, ROW_STYLE[row].color, 0)
    c:text("= " .. ROW_STYLE[row].pts .. " PTS", 168, y + 2,
           GREY, 17, "left")
  end
  c:text("DRAG TO MOVE, TAP TO FIRE", 200, 268, GREY, 15,
         "center")
  c:text("TAP TO PLAY", 200, 300, GREEN, 22, "center")
  if best > 0 then
    c:text("BEST " .. best, 200, 336, GREY, 15, "center")
  end
end

local function draw_over()
  c:rect(0, 0, 400, 400, BG, { fill = true })
  c:text("GAME OVER", 200, 120, WHITE, 34, "center")
  c:text("SCORE " .. score, 200, 178, WHITE, 20, "center")
  c:text("BEST " .. best, 200, 208, GREY, 16, "center")
  c:text("TAP TO PLAY AGAIN", 200, 262, GREEN, 19, "center")
end

local function draw_boom()
  c:rect(0, 0, 400, 400, BG, { fill = true })
  draw_hud_full()
  for _, v in ipairs(inv) do
    if v.alive then
      local x = ox + v.col * PITCH_X
      local y = oy + (v.row - 1) * PITCH_Y
      draw_invader(c, x, y, ROW_STYLE[v.row].color, leg)
      v.dx, v.dy, v.dleg = x, y, leg
    end
  end
  -- the cannon bursting: a ring of sparks
  for a = 0, 7 do
    local ang = a * math.pi / 4
    local sx = death_x + math.cos(ang) * 16
    local sy = CANNON_Y + 7 + math.sin(ang) * 10
    c:rect(sx - 2, sy - 2, 4, 4, YELLOW, { fill = true })
  end
  drawn_cx = nil
end

-- ---------- game flow ----------
local function game_over()
  state = "over"
  if score > best then
    best = score
    store.set("best", best)
  end
  draw_over()
end

local function start_game()
  score, lives, wave = 0, 3, 1
  cannon_x, target_x = 200, 200
  setup_wave()
  state = "play"
  draw_play_full()
end

local function cannon_hit()
  lives = lives - 1
  death_x = cannon_x
  sound.effect("explosion", { volume = 60 })
  state = "boom"
  boom_t = 0.85
  draw_boom()
end

-- ---------- update ----------
local function try_fire()
  if fire_cd > 0 or #bullets >= 2 then return end
  fire_cd = 0.24
  bullets[#bullets + 1] = { x = cannon_x, y = CANNON_Y - 10 }
  sound.effect("laser", { volume = 22 })
end

local function step_formation()
  leg = 1 - leg
  local nx = ox + dir * 7
  if nx < 10 or nx > 390 - FORM_W then
    dir = -dir
    oy = oy + 14
    nx = ox + dir * 7
  end
  ox = nx
  -- have they landed?
  local lowest = 0
  for _, v in ipairs(inv) do
    if v.alive and v.row > lowest then lowest = v.row end
  end
  if lowest > 0 and oy + (lowest - 1) * PITCH_Y + IH >= DANGER_Y then
    game_over()
  end
end

local function update(dt)
  -- cannon steering
  local dx = target_x - cannon_x
  local max_step = 380 * dt
  if dx > max_step then dx = max_step
  elseif dx < -max_step then dx = -max_step end
  cannon_x = cannon_x + dx
  if cannon_x < 34 then cannon_x = 34 end
  if cannon_x > 366 then cannon_x = 366 end

  -- firing
  if fire_cd > 0 then fire_cd = fire_cd - dt end
  if touching then try_fire() end

  -- bullets
  for _, b in ipairs(bullets) do
    b.y = b.y - 470 * dt
    if b.y < 30 then b.dead = true end
  end
  -- bombs
  for _, b in ipairs(bombs) do
    b.y = b.y + (175 + wave * 8) * dt
    if b.y > 382 then b.dead = true end
  end
  bomb_acc = bomb_acc + dt
  local bomb_every = math.max(0.5, 0.95 - wave * 0.05)
  if bomb_acc >= bomb_every and #bombs < 3 and alive_n > 0 then
    bomb_acc = 0
    local col = math.random(0, COLS - 1)
    local low = nil
    for _, v in ipairs(inv) do
      if v.alive and v.col == col
         and (not low or v.row > low.row) then low = v end
    end
    if low then
      bombs[#bombs + 1] = {
        x = ox + low.col * PITCH_X + IW / 2,
        y = oy + (low.row - 1) * PITCH_Y + IH,
      }
    end
  end

  -- formation march
  step_acc = step_acc + dt * 1000
  local base = math.max(340, 620 - (wave - 1) * 45)
  local step_ms = math.max(70, base * alive_n / (COLS * ROWS_N))
  if step_acc >= step_ms then
    step_acc = 0
    step_formation()
    if state ~= "play" then return end
  end

  -- collisions: bullets vs invaders and bombs
  for _, b in ipairs(bullets) do
    if not b.dead then
      for _, v in ipairs(inv) do
        if v.alive then
          local ix = ox + v.col * PITCH_X
          local iy = oy + (v.row - 1) * PITCH_Y
          if b.x >= ix - 1 and b.x <= ix + IW + 1
             and b.y >= iy - 2 and b.y <= iy + IH then
            v.alive = false
            alive_n = alive_n - 1
            score = score + ROW_STYLE[v.row].pts
            if score > best then best = score end
            b.dead = true
            sound.effect("hit", { volume = 45 })
            break
          end
        end
      end
      if not b.dead then
        for _, m in ipairs(bombs) do
          if not m.dead and math.abs(b.x - m.x) < 5
             and math.abs(b.y - m.y) < 9 then
            b.dead, m.dead = true, true
            sound.effect("tick", { volume = 30 })
            break
          end
        end
      end
    end
  end
  -- bombs vs cannon
  for _, m in ipairs(bombs) do
    if not m.dead and m.y + 8 >= CANNON_Y + 3
       and m.y <= CANNON_Y + 16
       and math.abs(m.x - cannon_x) <= 20 then
      m.dead = true
      cannon_hit()
      return
    end
  end
  -- wave cleared
  if alive_n <= 0 then
    wave = wave + 1
    sound.effect("win", { volume = 50 })
    setup_wave()
    draw_play_full()
  end
end

-- ---------- per-frame paint (partial, cover-based) ----------
local function draw()
  -- wave banner expiry first: its cover can erase
  -- invaders behind it, so force those to repaint below
  if banner_t <= 0 and banner_shown then
    banner_shown = false
    c:rect(105, 180, 190, 40, BG, { fill = true })
    for _, v in ipairs(inv) do
      if v.alive and v.dx then
        local x = ox + v.col * PITCH_X
        local y = oy + (v.row - 1) * PITCH_Y
        if x < 295 and x + IW > 105
           and y < 220 and y + IH > 180 then
          v.dx = nil
        end
      end
    end
  end
  -- invaders
  for _, v in ipairs(inv) do
    local x = ox + v.col * PITCH_X
    local y = oy + (v.row - 1) * PITCH_Y
    if v.alive then
      if v.dx ~= x or v.dy ~= y or v.dleg ~= leg then
        if v.dx then
          c:rect(v.dx - 2, v.dy - 2, IW + 4, IH + 4, BG,
                 { fill = true })
        end
        draw_invader(c, x, y, ROW_STYLE[v.row].color, leg)
        v.dx, v.dy, v.dleg = x, y, leg
      end
    elseif v.dx then
      c:rect(v.dx - 2, v.dy - 2, IW + 4, IH + 4, BG,
             { fill = true })
      v.dx = nil
    end
  end
  -- cannon
  if drawn_cx ~= cannon_x then
    if drawn_cx then
      c:rect(drawn_cx - 20, CANNON_Y - 2, 40, 18, BG,
             { fill = true })
    end
    draw_cannon(c, cannon_x)
    drawn_cx = cannon_x
  end
  -- bullets
  for i = #bullets, 1, -1 do
    local b = bullets[i]
    if b.dead then
      if b.px then
        c:rect(b.px - 3, b.py - 2, 7, 14, BG, { fill = true })
      end
      table.remove(bullets, i)
    elseif b.px ~= b.x or b.py ~= b.y then
      if b.px then
        c:rect(b.px - 3, b.py - 2, 7, 14, BG, { fill = true })
      end
      c:rect(b.x - 1, b.y, 3, 10, WHITE, { fill = true })
      b.px, b.py = b.x, b.y
    end
  end
  -- bombs
  for i = #bombs, 1, -1 do
    local m = bombs[i]
    if m.dead then
      if m.px then
        c:rect(m.px - 4, m.py - 2, 8, 12, BG, { fill = true })
      end
      table.remove(bombs, i)
    elseif m.px ~= m.x or m.py ~= m.y then
      if m.px then
        c:rect(m.px - 4, m.py - 2, 8, 12, BG, { fill = true })
      end
      c:rect(m.x - 2, m.y, 4, 8, YELLOW, { fill = true })
      m.px, m.py = m.x, m.y
    end
  end
  -- HUD, only the strip that changed
  if drawn_score ~= score then
    c:rect(8, 4, 150, 28, BG, { fill = true })
    c:text("SCORE " .. score, 14, 8, WHITE, 15, "left")
    drawn_score = score
  end
  if drawn_wave ~= wave then
    c:rect(150, 4, 100, 28, BG, { fill = true })
    c:text("WAVE " .. wave, 200, 8, GREY, 15, "center")
    drawn_wave = wave
  end
  if drawn_lives ~= lives then
    c:rect(300, 4, 96, 28, BG, { fill = true })
    for i = 0, lives - 1 do
      c:rect(382 - i * 22, 11, 16, 8, GREEN, { fill = true })
    end
    drawn_lives = lives
  end
end

-- ---------- input ----------
local function handle_touch(kind, x, y)
  if kind == "down" then
    touching = true
    target_x = x
    if state == "ready" or state == "over" then
      sound.effect("click", { volume = 20 })
      start_game()
    elseif state == "play" then
      try_fire()
    end
  elseif kind == "move" then
    target_x = x
  elseif kind == "up" then
    touching = false
  end
end

-- launcher icon: one big invader on black
do
  local ic = ui.canvas{ w = 96, h = 96, color = BG }
  draw_invader(ic, 18, 28, GREEN, 0, 2)
  app.icon(ic)
  ic:delete()
end

ui.background(BG)

c = ui.canvas{
  w = 400, h = 400, color = BG,
  on_touch = function(kind, x, y) handle_touch(kind, x, y) end,
}
c:partial(true)

draw_ready()

local last_t = time.now()
ui.every(16, function()
  local now = time.now()
  local dt = now - last_t
  last_t = now
  if dt > 0.05 then dt = 0.05 end
  if banner_t > 0 then
    banner_t = banner_t - dt
    if banner_t > 0 then banner_shown = true end
  end
  if state == "play" then
    update(dt)
    if state == "play" then draw() end
  elseif state == "boom" then
    boom_t = boom_t - dt
    if boom_t <= 0 then
      if lives <= 0 then
        game_over()
      else
        bullets, bombs = {}, {}
        state = "play"
        draw_play_full()
      end
    end
  end
end)

print("invaders ready, best " .. best)
