-- Vela Jump: a doodle-jump climber, drawn for the
-- round screen, with Vela the puppy in the doodler's
-- place and her propeller hat spinning at every
-- bounce. She bounces on platforms forever; drag to
-- steer her left and right. She glides after your
-- finger with a little weight to her, easing in and
-- out instead of snapping, and the walls stop her:
-- she never wraps around to the other side. Thread
-- the green and blue platforms, mind the brown ones,
-- which break, and catch a spring for a real launch.
-- The score is the height climbed, in metres; the
-- best is kept in the store.
--
-- Controls: touch and drag to steer. Tap to start,
-- tap to play again.
--
-- Rendering follows the house pattern for games: one
-- 400x400 canvas at zoom 1 in partial mode. The scene
-- is painted once, then each frame covers the movers'
-- old spots and draws them at the new ones.

local BG = 0xE9F6F9
local GREEN = 0x73BE2E
local GREEN_DK = 0x4E7D1E
local BLUE = 0x3E7CB1
local BLUE_DK = 0x2A567C
local BROWN = 0xB08954
local BROWN_DK = 0x7D5F36
local SPRING = 0xE84B4B
local COIL = 0x9AA5AD
local FUR = 0xEFC98F
local EAR = 0xB07B4F
local SNOUT = 0xFBEED3
local PROP = 0xE84B4B
local CAP = 0x3E7CB1
local INK = 0x20242A

local GRAV, BOUNCE, SPRING_V = 1500, -880, -1420
local PLW, PLH = 58, 12
local SCROLL_Y = 150

local best = store.get("best") or 0   -- metres

local state = "ready"   -- ready | play | over
local px, py, vy = 200, 282, 0
local hvx = 0   -- her horizontal velocity (steering)
local tx = 200
local plats = {}
local score_px = 0
local prop_a, prop_spin = 0, 0
local t_now, over_t = 0, 0
local drawn_m = -1
local score_dirty = true
local obx, oby = nil, nil   -- Vela's last drawn spot

local c = nil   -- set below, everything draws on it

local function metres()
  return math.floor(score_px / 50)
end

-- ---------- Vela, front-facing ----------
local function draw_vela(cv, x, y)
  cv:circle(x - 14, y - 3, 8, EAR, { fill = true })
  cv:circle(x + 14, y - 3, 8, EAR, { fill = true })
  cv:circle(x, y, 17, FUR, { fill = true })
  cv:circle(x, y - 13, 6, CAP, { fill = true })
  cv:line(x, y - 17, x, y - 21, EAR, 3)
  local bc = math.cos(prop_a) * 12
  local bs = math.sin(prop_a) * 4
  cv:line(x - bc, y - 22 - bs, x + bc, y - 22 + bs, PROP, 4)
  cv:circle(x, y - 22, 3, CAP, { fill = true })
  cv:circle(x - 6.5, y - 2, 2.6, INK, { fill = true })
  cv:circle(x + 6.5, y - 2, 2.6, INK, { fill = true })
  cv:circle(x, y + 6, 8, SNOUT, { fill = true })
  cv:circle(x, y + 3.5, 3.2, INK, { fill = true })
  cv:circle(x - 9, y + 17, 4.5, EAR, { fill = true })
  cv:circle(x + 9, y + 17, 4.5, EAR, { fill = true })
end

-- ---------- platforms ----------
local function draw_plat(cv, p)
  local col, dk = GREEN, GREEN_DK
  if p.kind == "move" then
    col, dk = BLUE, BLUE_DK
  elseif p.kind == "break" then
    col, dk = BROWN, BROWN_DK
  end
  cv:rect(p.x, p.y, PLW, PLH, col, { fill = true, radius = 6 })
  cv:line(p.x + 5, p.y + PLH - 2, p.x + PLW - 5, p.y + PLH - 2,
          dk, 2)
  if p.kind == "break" then
    cv:line(p.x + PLW / 2 - 6, p.y + 2, p.x + PLW / 2 + 2,
            p.y + PLH - 2, dk, 2)
    cv:line(p.x + PLW / 2 + 2, p.y + PLH - 2, p.x + PLW / 2 + 8,
            p.y + 3, dk, 2)
  end
  if p.spring then
    local sx = p.x + PLW / 2
    cv:rect(sx - 7, p.y - 4, 14, 4, COIL, { fill = true })
    cv:rect(sx - 8, p.y - 8, 16, 4, SPRING, { fill = true })
  end
end

local function cover_plat(cv, p, ox, oy)
  cv:rect(ox - 2, oy - 10, PLW + 4, PLH + 13, BG, { fill = true })
end

local function gen_platform(y)
  local kind = "norm"
  local r = math.random()
  if r < 0.20 then
    kind = "break"
  elseif r < 0.42 then
    kind = "move"
  end
  local p = {
    x = math.random(8, 334), y = y, kind = kind,
    spring = kind == "norm" and math.random() < 0.12,
    state = "idle", bvy = 0, vx = 0,
  }
  if kind == "move" then
    p.vx = math.random(40, 70)
           * (math.random() < 0.5 and -1 or 1)
  end
  plats[#plats + 1] = p
  return p
end

local function reset_world()
  plats = {}
  plats[1] = {
    x = 171, y = 308, kind = "norm", spring = false,
    state = "idle", bvy = 0, vx = 0,
  }
  local y = 308
  while y > -40 do
    y = y - math.random(58, 90)
    gen_platform(y)
  end
  px, py, vy = 200, 282, 0
  hvx = 0
  tx = 200
  score_px = 0
  drawn_m = -1
  score_dirty = true
end

-- ---------- screens ----------
local function draw_score()
  local s = metres() .. " m"
  local w = #s * 16 + 18
  c:rect(200 - w / 2, 16, w, 36, BG, { fill = true })
  c:text(s, 200, 20, INK, 28, "center")
  drawn_m = metres()
  score_dirty = false
end

local function draw_world_full()
  c:rect(0, 0, 400, 400, BG, { fill = true })
  for _, p in ipairs(plats) do
    draw_plat(c, p)
    p.dx, p.dy = p.x, p.y
  end
  draw_vela(c, px, py)
  obx, oby = px, py
  score_dirty = true
  draw_score()
end

local function draw_ready_full()
  c:rect(0, 0, 400, 400, BG, { fill = true })
  for _, p in ipairs(plats) do
    draw_plat(c, p)
    p.dx, p.dy = p.x, p.y
  end
  c:text("VELA JUMP", 200, 84, INK, 28, "center")
  draw_vela(c, 200, 282)
  obx, oby = 200, 282
  c:text("DRAG TO STEER", 200, 352, INK, 20, "center")
  if best > 0 then
    c:text("BEST " .. best .. " m", 200, 380, INK, 16, "center")
  end
end

local function draw_over_full()
  c:rect(0, 0, 400, 400, BG, { fill = true })
  c:text("GAME OVER", 200, 96, INK, 28, "center")
  c:text(metres() .. " m", 200, 140, INK, 48, "center")
  c:text("BEST " .. best .. " m", 200, 216, INK, 16, "center")
  local p = { x = 171, y = 322, kind = "norm", spring = false }
  draw_plat(c, p)
  draw_vela(c, 200, 300)
  c:text("TAP TO PLAY AGAIN", 200, 362, INK, 20, "center")
  obx, oby = nil, nil
end

-- ---------- game flow ----------
local function start_game()
  reset_world()
  state = "play"
  draw_world_full()
  vy = BOUNCE
  prop_spin = 0.35
  sound.effect("jump", { volume = 18 })
end

local function game_over()
  state = "over"
  over_t = 0
  local m = metres()
  if m > best then
    best = m
    store.set("best", best)
  end
  sound.effect("lose", { volume = 40 })
  draw_over_full()
end

-- ---------- update ----------
local function update_play(dt)
  -- steering: she has weight. Her horizontal speed
  -- leans toward a target speed that grows with the
  -- distance to your finger and tops out well under
  -- the old snap, so she eases out, glides, and
  -- settles instead of teleporting after the drag.
  -- The walls stop her; there is no wrap-around.
  local dx = tx - px
  local want = dx * 6
  if want > 330 then want = 330
  elseif want < -330 then want = -330 end
  if dx > -2 and dx < 2 then want = 0 end
  local dv = want - hvx
  local acc = 1700 * dt
  if dv > acc then hvx = hvx + acc
  elseif dv < -acc then hvx = hvx - acc
  else hvx = want end
  px = px + hvx * dt
  if px < 30 then px = 30 hvx = 0
  elseif px > 370 then px = 370 hvx = 0 end
  -- vertical motion
  local prev_feet = py + 20
  vy = math.min(vy + GRAV * dt, 1100)
  py = py + vy * dt
  local feet = py + 20
  -- platforms with lives of their own
  for _, p in ipairs(plats) do
    if p.kind == "move" and p.state == "idle" then
      p.x = p.x + p.vx * dt
      if p.x < 8 then p.x = 8 p.vx = -p.vx end
      if p.x > 400 - PLW - 8 then
        p.x = 400 - PLW - 8
        p.vx = -p.vx
      end
    elseif p.state == "falling" then
      p.bvy = p.bvy + GRAV * 0.7 * dt
      p.y = p.y + p.bvy * dt
    end
  end
  -- bounce, only while falling onto a platform
  if vy > 0 then
    for _, p in ipairs(plats) do
      if p.state == "idle" and prev_feet <= p.y + 8
         and feet >= p.y
         and px + 13 > p.x and px - 13 < p.x + PLW then
        if p.kind == "break" then
          p.state = "falling"
          p.bvy = 40
          sound.effect("click", { volume = 30 })
        else
          local on_spring = p.spring
            and px > p.x + PLW / 2 - 11
            and px < p.x + PLW / 2 + 11
          if on_spring then
            vy = SPRING_V
            sound.effect("powerup", { volume = 45 })
          else
            vy = BOUNCE
            sound.effect("jump", { volume = 18 })
          end
          prop_spin = 0.35
          py = p.y - 20
        end
        break
      end
    end
  end
  -- the camera: keep her at SCROLL_Y while she climbs
  if py < SCROLL_Y and vy < 0 then
    local dy = SCROLL_Y - py
    py = SCROLL_Y
    score_px = score_px + dy
    for _, p in ipairs(plats) do p.y = p.y + dy end
  end
  -- cull below, spawn above
  local top_y = math.huge
  for _, p in ipairs(plats) do
    if p.y > 412 then p.dead = true end
    if not p.dead and p.y < top_y then top_y = p.y end
  end
  while top_y > -30 do
    top_y = top_y - math.random(58, 90)
            - math.min(14, score_px / 4000)
    gen_platform(top_y)
  end
  if py - 24 > 400 then game_over() end
end

-- ---------- per-frame paint (partial, cover-based) ----------
local function draw_frame()
  for i = #plats, 1, -1 do
    local p = plats[i]
    if p.dx and (p.dead or p.dx ~= p.x or p.dy ~= p.y) then
      cover_plat(c, p, p.dx, p.dy)
      if p.dy - 10 < 56 and p.dx + PLW + 2 > 140
         and p.dx - 2 < 260 then
        score_dirty = true
      end
      p.dx, p.dy = nil, nil
    end
    if p.dead then
      table.remove(plats, i)
    elseif p.dx == nil then
      draw_plat(c, p)
      if p.y - 10 < 56 and p.x + PLW + 2 > 140
         and p.x - 2 < 260 then
        score_dirty = true
      end
      p.dx, p.dy = p.x, p.y
    end
  end
  if obx then
    c:rect(obx - 26, oby - 28, 52, 52, BG, { fill = true })
    if oby - 28 < 56 then score_dirty = true end
  end
  draw_vela(c, px, py)
  obx, oby = px, py
  if py - 28 < 56 then score_dirty = true end
  if score_dirty or drawn_m ~= metres() then draw_score() end
end

local function draw_ready_frame()
  local nb = 282 + math.sin(t_now * 2.2) * 5
  if obx then
    c:rect(obx - 26, oby - 28, 52, 52, BG, { fill = true })
  end
  draw_vela(c, 200, nb)
  obx, oby = 200, nb
end

-- ---------- input ----------
local function handle_touch(kind, x, y)
  if kind == "down" or kind == "move" then
    tx = math.max(30, math.min(370, x))
  elseif kind == "up" then
    tx = px   -- finger lifted: she eases to a stop here
  end
  if kind ~= "down" then return end
  if state == "ready" then
    start_game()
  elseif state == "over" and over_t > 0.6 then
    start_game()
  end
end

-- launcher icon: Vela over a platform, propeller up
do
  local ic = ui.canvas{ w = 96, h = 96, color = BG }
  prop_a = 0.5
  draw_vela(ic, 48, 42)
  local p = { x = 19, y = 68, kind = "norm", spring = false }
  draw_plat(ic, p)
  app.icon(ic)
  ic:delete()
end

ui.background(BG)

c = ui.canvas{
  w = 400, h = 400, color = BG,
  on_touch = function(kind, x, y) handle_touch(kind, x, y) end,
}
c:partial(true)

reset_world()
draw_ready_full()

local last_t = time.now()
ui.every(16, function()
  local now = time.now()
  local dt = now - last_t
  last_t = now
  if dt > 0.05 then dt = 0.05 end
  t_now = t_now + dt
  if prop_spin > 0 then
    prop_spin = prop_spin - dt
    prop_a = prop_a + dt * 46
  else
    prop_a = prop_a + dt * 9
  end
  if state == "ready" then
    draw_ready_frame()
  elseif state == "play" then
    update_play(dt)
    if state == "play" then draw_frame() end
  elseif state == "over" then
    over_t = over_t + dt
  end
end)

print("doodle ready, best " .. best)
