-- Asteroids: the classic vector shooter, drawn as pure
-- contours like the original arcade cabinet: the ship is
-- an outline, every asteroid is an irregular outline
-- polygon that drifts and tumbles, nothing is filled.
--
-- Four sizes of asteroid. A destroyed one splits into
-- two of the next size down (so a huge one ends as four
-- small ones through its children), and the two biggest
-- sizes sometimes split into three instead, for variety.
-- The smallest size just bursts into shards.
--
-- Controls are one finger: touch and hold, the ship
-- turns toward your finger, thrusts and fires for as
-- long as you hold. Release and it drifts. Three lives,
-- a wave more crowded than the last, best score kept in
-- the store.
--
-- Rendering: one 400x400 canvas at zoom 1 in partial
-- mode. Everything drifts every frame, so each frame
-- covers every mover's old bounding box and redraws its
-- outline at the new spot.

local BG = 0x05060A
local WHITE = 0xF2F2F2
local GREY = 0x8A8F98
local ROCK = 0xD8D8D8
local GREEN = 0x39D353
local YELLOW = 0xFFD60A

-- size tiers, biggest first: radius, score, base speed
local TIERS = {
  { r = 40, pts = 20,  spd = 26 },
  { r = 29, pts = 50,  spd = 38 },
  { r = 19, pts = 100, spd = 56 },
  { r = 11, pts = 200, spd = 80 },
}
local MAX_ROCKS = 40

local best = store.get("best") or 0

-- ---------- drawing ----------
local function draw_ship(cv, x, y, ang, flame)
  local ca, sa = math.cos(ang), math.sin(ang)
  local pts = { {13, 0}, {-9, 8}, {-5, 0}, {-9, -8} }
  local wx, wy = {}, {}
  for i, p in ipairs(pts) do
    wx[i] = x + p[1] * ca - p[2] * sa
    wy[i] = y + p[1] * sa + p[2] * ca
  end
  for i = 1, 4 do
    local j = i % 4 + 1
    cv:line(wx[i], wy[i], wx[j], wy[j], GREEN, 2)
  end
  if flame ~= nil then
    local bx = x - 5 * ca
    local by = y - 5 * sa
    local len = flame == 1 and 15 or 10
    local tx = x - (5 + len) * ca
    local ty = y - (5 + len) * sa
    local px, py = -sa * 3.5, ca * 3.5
    cv:line(bx + px, by + py, tx, ty, YELLOW, 2)
    cv:line(bx - px, by - py, tx, ty, YELLOW, 2)
  end
end

local function draw_rock(cv, rk)
  local ca, sa = math.cos(rk.ang), math.sin(rk.ang)
  local n = #rk.verts
  local fx, fy
  local lx, ly
  for i = 1, n do
    local v = rk.verts[i]
    local wx = rk.x + (v[1] * ca - v[2] * sa) * rk.r
    local wy = rk.y + (v[1] * sa + v[2] * ca) * rk.r
    if i == 1 then fx, fy = wx, wy else
      cv:line(lx, ly, wx, wy, ROCK, 2)
    end
    lx, ly = wx, wy
  end
  cv:line(lx, ly, fx, fy, ROCK, 2)
end

local function make_verts()
  local n = 8 + math.random(0, 3)
  local verts = {}
  for i = 1, n do
    local a = (i - 1) / n * math.pi * 2
    local j = 0.74 + math.random() * 0.42
    verts[i] = { math.cos(a) * j, math.sin(a) * j }
  end
  return verts
end

-- ---------- state ----------
local state = "ready"     -- ready | play | boom | over
local score, lives, wave = 0, 3, 1
local rocks, bullets, parts = {}, {}, {}
local ghosts = {}   -- last spots of destroyed rocks, to erase
local ship = nil
local touching = false
local touch_x, touch_y = 200, 120
local fire_cd, boom_t, banner_t = 0, 0, 0
local banner_shown = false
local flick = 0

local drawn_score, drawn_wave, drawn_lives = -1, -1, -1

local function spawn_rock(x, y, tier, pvx, pvy)
  local t = TIERS[tier]
  local a = math.random() * math.pi * 2
  local s = t.spd * (0.75 + math.random() * 0.5)
  rocks[#rocks + 1] = {
    x = x, y = y, tier = tier, r = t.r,
    vx = (pvx or 0) + math.cos(a) * s,
    vy = (pvy or 0) + math.sin(a) * s,
    ang = math.random() * math.pi * 2,
    spin = (math.random() - 0.5) * 1.4,
    verts = make_verts(),
  }
end

local function spawn_wave()
  rocks = {}
  local n = 3 + wave
  for _ = 1, n do
    local x, y
    repeat
      local edge = math.random(0, 3)
      if edge == 0 then x, y = math.random(0, 400), 30
      elseif edge == 1 then x, y = math.random(0, 400), 370
      elseif edge == 2 then x, y = 30, math.random(0, 400)
      else x, y = 370, math.random(0, 400) end
    until (x - 200) ^ 2 + (y - 200) ^ 2 > 120 * 120
    spawn_rock(x, y, 1)
  end
  banner_t = 1.1
end

local function burst(x, y, col, n, spd)
  for _ = 1, n do
    if #parts >= 70 then break end
    local a = math.random() * math.pi * 2
    local s = spd * (0.3 + math.random() * 0.9)
    parts[#parts + 1] = {
      x = x, y = y,
      vx = math.cos(a) * s, vy = math.sin(a) * s,
      life = 0.35 + math.random() * 0.3, col = col,
    }
  end
end

local function new_ship()
  ship = { x = 200, y = 200, vx = 0, vy = 0, ang = -math.pi / 2,
           invuln = 2.5 }
end

-- ---------- screens ----------
local c = nil

local function draw_hud_full()
  c:text("SCORE " .. score, 14, 8, WHITE, 15, "left")
  c:text("WAVE " .. wave, 200, 8, GREY, 15, "center")
  for i = 0, lives - 1 do
    draw_ship(c, 378 - i * 26, 19, -math.pi / 2)
  end
  drawn_score, drawn_wave, drawn_lives = score, wave, lives
end

local function draw_play_full()
  c:rect(0, 0, 400, 400, BG, { fill = true })
  draw_hud_full()
  for _, rk in ipairs(rocks) do
    draw_rock(c, rk)
    rk.px, rk.py = rk.x, rk.y
  end
  if ship then draw_ship(c, ship.x, ship.y, ship.ang) end
  if banner_t > 0 then
    c:text("WAVE " .. wave, 200, 188, WHITE, 30, "center")
  end
end

local function draw_ready()
  c:rect(0, 0, 400, 400, BG, { fill = true })
  c:text("ASTEROIDS", 200, 48, WHITE, 36, "center")
  local demo = { x = 118, y = 148, r = 34, ang = 0.5,
                 verts = make_verts() }
  draw_rock(c, demo)
  draw_ship(c, 268, 152, -0.6, 1)
  c:text("BIG ROCKS SPLIT INTO SMALLER ONES", 200, 224,
         GREY, 14, "center")
  c:text("HOLD TO FLY AND FIRE", 200, 262, GREY, 15, "center")
  c:text("TAP TO PLAY", 200, 296, GREEN, 22, "center")
  if best > 0 then
    c:text("BEST " .. best, 200, 332, GREY, 15, "center")
  end
end

local function draw_over()
  c:rect(0, 0, 400, 400, BG, { fill = true })
  c:text("GAME OVER", 200, 120, WHITE, 34, "center")
  c:text("SCORE " .. score, 200, 178, WHITE, 20, "center")
  c:text("BEST " .. best, 200, 208, GREY, 16, "center")
  c:text("TAP TO PLAY AGAIN", 200, 262, GREEN, 19, "center")
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
  bullets, parts, ghosts = {}, {}, {}
  new_ship()
  spawn_wave()
  state = "play"
  draw_play_full()
end

local function ship_hit()
  lives = lives - 1
  burst(ship.x, ship.y, GREEN, 14, 130)
  burst(ship.x, ship.y, WHITE, 8, 90)
  sound.effect("explosion", { volume = 60 })
  if ship.px then
    c:rect(ship.px - 17, ship.py - 17, 34, 34, BG,
           { fill = true })
  end
  ship = nil
  bullets = {}
  state = "boom"
  boom_t = 1.0
end

local function break_rock(rk)
  local t = TIERS[rk.tier]
  if rk.px then
    ghosts[#ghosts + 1] = { x = rk.px, y = rk.py, r = rk.r }
  end
  score = score + t.pts
  if score > best then best = score end
  burst(rk.x, rk.y, ROCK, rk.tier == 4 and 6 or 9, 110)
  sound.effect(rk.tier >= 3 and "hit" or "explosion",
               { volume = rk.tier >= 3 and 40 or 50 })
  if rk.tier < 4 then
    local n = 2
    if rk.tier <= 2 and math.random() < 0.3 then n = 3 end
    while #rocks + n - 1 > MAX_ROCKS and n > 0 do n = n - 1 end
    for _ = 1, n do
      spawn_rock(rk.x, rk.y, rk.tier + 1, rk.vx * 0.5, rk.vy * 0.5)
    end
  end
end

-- ---------- update ----------
local function wrap(e, m)
  if e.x < -m then e.x = e.x + 400 + 2 * m
  elseif e.x > 400 + m then e.x = e.x - 400 - 2 * m end
  if e.y < -m then e.y = e.y + 400 + 2 * m
  elseif e.y > 400 + m then e.y = e.y - 400 - 2 * m end
end

local function update(dt)
  -- rocks always drift and tumble
  for _, rk in ipairs(rocks) do
    rk.x = rk.x + rk.vx * dt
    rk.y = rk.y + rk.vy * dt
    rk.ang = rk.ang + rk.spin * dt
    wrap(rk, rk.r)
  end
  -- shards (expired ones are flagged; the paint pass
  -- erases their last spot before dropping them)
  for _, p in ipairs(parts) do
    p.x = p.x + p.vx * dt
    p.y = p.y + p.vy * dt
    p.life = p.life - dt
    if p.life <= 0 then p.dead = true end
  end
  if state ~= "play" then return end

  -- ship
  if ship then
    if ship.invuln > 0 then ship.invuln = ship.invuln - dt end
    if touching then
      -- turn the nose toward the finger, at most 10 rad/s
      local dx = touch_x - ship.x
      local dy = touch_y - ship.y
      local d = math.sqrt(dx * dx + dy * dy)
      if d > 4 then
        local hx, hy = math.cos(ship.ang), math.sin(ship.ang)
        local ux, uy = dx / d, dy / d
        local cross = hx * uy - hy * ux
        local dot = hx * ux + hy * uy
        if dot < -1 then dot = -1 elseif dot > 1 then dot = 1 end
        local turn = math.acos(dot)
        local max_turn = 10 * dt
        if turn > max_turn then turn = max_turn end
        if cross < 0 then turn = -turn end
        ship.ang = ship.ang + turn
      end
      local ca, sa = math.cos(ship.ang), math.sin(ship.ang)
      ship.vx = ship.vx + ca * 300 * dt
      ship.vy = ship.vy + sa * 300 * dt
      -- fire
      if fire_cd > 0 then fire_cd = fire_cd - dt end
      if fire_cd <= 0 and #bullets < 6 then
        fire_cd = 0.15
        bullets[#bullets + 1] = {
          x = ship.x + ca * 14, y = ship.y + sa * 14,
          vx = ca * 430, vy = sa * 430, life = 0.85,
        }
        sound.effect("laser", { volume = 18 })
      end
    end
    local damp = 1 - 0.45 * dt
    ship.vx = ship.vx * damp
    ship.vy = ship.vy * damp
    local sp = math.sqrt(ship.vx ^ 2 + ship.vy ^ 2)
    if sp > 280 then
      ship.vx = ship.vx * 280 / sp
      ship.vy = ship.vy * 280 / sp
    end
    ship.x = ship.x + ship.vx * dt
    ship.y = ship.y + ship.vy * dt
    wrap(ship, 14)
  end
  if fire_cd > 0 and not touching then fire_cd = fire_cd - dt end

  -- bullets
  for i = #bullets, 1, -1 do
    local b = bullets[i]
    b.x = b.x + b.vx * dt
    b.y = b.y + b.vy * dt
    b.life = b.life - dt
    wrap(b, 4)
    if b.life <= 0 then b.dead = true end
  end
  -- bullet vs rock
  for _, b in ipairs(bullets) do
    if not b.dead then
      for j = #rocks, 1, -1 do
        local rk = rocks[j]
        local dx = b.x - rk.x
        local dy = b.y - rk.y
        if dx * dx + dy * dy < (rk.r + 2) * (rk.r + 2) then
          b.dead = true
          rk.dead = true
          break_rock(rk)
          table.remove(rocks, j)
          break
        end
      end
    end
  end
  -- ship vs rock
  if ship and ship.invuln <= 0 then
    for _, rk in ipairs(rocks) do
      local dx = ship.x - rk.x
      local dy = ship.y - rk.y
      local rr = rk.r * 0.92 + 10
      if dx * dx + dy * dy < rr * rr then
        ship_hit()
        return
      end
    end
  end
  -- wave cleared
  if #rocks == 0 then
    wave = wave + 1
    sound.effect("win", { volume = 50 })
    spawn_wave()
    draw_play_full()
  end
end

-- ---------- per-frame paint (partial, cover-based) ----------
local function draw()
  local hud_dirty = false
  -- wave banner expiry first (its cover can erase rocks)
  if banner_t <= 0 and banner_shown then
    banner_shown = false
    c:rect(105, 180, 190, 40, BG, { fill = true })
    for _, rk in ipairs(rocks) do rk.px = nil end
  end
  -- erase the last outlines of rocks destroyed this frame
  for _, g in ipairs(ghosts) do
    c:rect(g.x - g.r - 4, g.y - g.r - 4, g.r * 2 + 8,
           g.r * 2 + 8, BG, { fill = true })
    if g.y - g.r - 4 < 36 then hud_dirty = true end
  end
  ghosts = {}
  -- rocks
  for _, rk in ipairs(rocks) do
    if rk.px then
      c:rect(rk.px - rk.r - 4, rk.py - rk.r - 4,
             rk.r * 2 + 8, rk.r * 2 + 8, BG, { fill = true })
      if rk.py - rk.r - 4 < 36 then hud_dirty = true end
    end
    draw_rock(c, rk)
    rk.px, rk.py = rk.x, rk.y
    if rk.y - rk.r - 4 < 36 then hud_dirty = true end
  end
  -- bullets
  for i = #bullets, 1, -1 do
    local b = bullets[i]
    if b.px then
      c:rect(b.px - 3, b.py - 3, 7, 7, BG, { fill = true })
      if b.py < 40 then hud_dirty = true end
    end
    if b.dead then
      table.remove(bullets, i)
    else
      c:rect(b.x - 1, b.y - 1, 3, 3, WHITE, { fill = true })
      b.px, b.py = b.x, b.y
    end
  end
  -- shards
  for i = #parts, 1, -1 do
    local p = parts[i]
    if p.px then
      c:rect(p.px - 3, p.py - 3, 7, 7, BG, { fill = true })
    end
    if p.dead then
      table.remove(parts, i)
    else
      c:line(p.x, p.y, p.x - p.vx * 0.03, p.y - p.vy * 0.03,
             p.col, 2)
      p.px, p.py = p.x, p.y
    end
  end
  -- ship (blinks while invulnerable)
  if ship then
    if ship.px then
      c:rect(ship.px - 17, ship.py - 17, 34, 34, BG,
             { fill = true })
    end
    local blink_off = ship.invuln > 0
      and math.floor(ship.invuln / 0.18) % 2 == 1
    if not blink_off then
      local flame = nil
      if touching then flame = flick end
      draw_ship(c, ship.x, ship.y, ship.ang, flame)
    end
    ship.px, ship.py = ship.x, ship.y
  end
  -- HUD strips that changed, or that a rock crossed
  if hud_dirty or drawn_score ~= score then
    c:rect(8, 4, 150, 28, BG, { fill = true })
    c:text("SCORE " .. score, 14, 8, WHITE, 15, "left")
    drawn_score = score
  end
  if hud_dirty or drawn_wave ~= wave then
    c:rect(150, 4, 100, 28, BG, { fill = true })
    c:text("WAVE " .. wave, 200, 8, GREY, 15, "center")
    drawn_wave = wave
  end
  if hud_dirty or drawn_lives ~= lives then
    c:rect(296, 4, 100, 28, BG, { fill = true })
    for i = 0, lives - 1 do
      draw_ship(c, 378 - i * 26, 19, -math.pi / 2)
    end
    drawn_lives = lives
  end
end

-- ---------- input ----------
local function handle_touch(kind, x, y)
  if kind == "down" then
    touching = true
    touch_x, touch_y = x, y
    if state == "ready" or state == "over" then
      sound.effect("click", { volume = 20 })
      start_game()
    end
  elseif kind == "move" then
    touch_x, touch_y = x, y
  elseif kind == "up" then
    touching = false
  end
end

-- launcher icon: a rock and the ship, contours only
do
  local ic = ui.canvas{ w = 96, h = 96, color = BG }
  local demo = { x = 58, y = 36, r = 24, ang = 0.4,
                 verts = make_verts() }
  draw_rock(ic, demo)
  draw_ship(ic, 34, 66, -0.7, 1)
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
  flick = 1 - flick
  if banner_t > 0 then
    banner_t = banner_t - dt
    if banner_t > 0 then banner_shown = true end
  end
  if state == "play" or state == "boom" then
    update(dt)
    if state == "play" then
      draw()
    elseif state == "boom" then
      -- rocks keep drifting behind the burst
      draw()
      boom_t = boom_t - dt
      if boom_t <= 0 then
        if lives <= 0 then
          game_over()
        else
          -- respawn when the centre is clear, or anyway
          -- after a grace period, blinking
          local clear = true
          for _, rk in ipairs(rocks) do
            local dx = rk.x - 200
            local dy = rk.y - 200
            if dx * dx + dy * dy < 75 * 75 then
              clear = false break
            end
          end
          if clear or boom_t < -2.2 then
            new_ship()
            ship.px = nil
            state = "play"
            draw_play_full()
          end
        end
      end
    end
  end
end)

print("asteroids ready, best " .. best)
