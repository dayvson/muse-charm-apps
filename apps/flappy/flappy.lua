-- Flappy Vela: the one-button arcade classic, drawn
-- for the round screen, with Vela the puppy in the
-- bird's place. She wears a propeller hat, her own
-- little wind device: it spins hard while she climbs
-- and idles while she glides. Pipes slide in from the
-- right with a gap to thread; one point per pipe
-- cleared, best score kept in the store.
--
-- Controls: tap anywhere to flap. Tap to start, tap
-- to play again.
--
-- Rendering follows the house pattern for games: one
-- 400x400 canvas at zoom 1 in partial mode. The scene
-- is painted once, then each frame covers the movers'
-- old spots and draws them at the new ones.

local SKY = 0x70C5CE
local PIPE = 0x73BE2E
local PIPE_DK = 0x4E7D1E
local SAND = 0xDED895
local GRASS = 0x74BF2E
local FUR = 0xEFC98F
local EAR = 0xB07B4F
local SNOUT = 0xFBEED3
local PROP = 0xE84B4B
local CAP = 0x3E7CB1
local TONGUE = 0xF28BA8
local WHITE = 0xFFFFFF
local INK = 0x20242A
local SHADOW = 0x4E8D9C

local GROUND_Y = 356
local BX, BR = 140, 17
local GRAV, FLAP, MAXFALL = 1500, -440, 640
local PW, GAP = 56, 148
local SPACING = 226

local best = store.get("best") or 0

local state = "ready"   -- ready | play | fall | over
local score = 0
local by, vy, ang = 190, 0, 0
local prop_a = 0
local pipes = {}
local t_now, over_t = 0, 0
local drawn_score = -1
local score_dirty = true
local pbx, pby = nil, nil   -- Vela's last drawn spot

local c = nil   -- set below, everything draws on it

local function speed()
  return math.min(118 + score * 2, 170)
end

-- ---------- drawing ----------
-- Vela in profile, facing right: golden fur, one
-- floppy ear, a snout, and her propeller hat. The
-- propeller spins around its hub; prop_a is advanced
-- by the frame loop, fast while she climbs.
local function draw_vela(cv, x, y, a, ear_up)
  local ca, sa = math.cos(a), math.sin(a)
  local function P(dx, dy)
    return x + dx * ca - dy * sa, y + dx * sa + dy * ca
  end
  -- propeller mast and blades, behind the head
  local m1x, m1y = P(0, -15)
  local m2x, m2y = P(0, -22)
  cv:line(m1x, m1y, m2x, m2y, EAR, 3)
  local bc = math.cos(prop_a) * 13
  local bs = math.sin(prop_a) * 4.5
  cv:line(m2x - bc, m2y - bs, m2x + bc, m2y + bs, PROP, 4)
  -- body and cap
  cv:circle(x, y, BR, FUR, { fill = true })
  local cx2, cy2 = P(-1, -13)
  cv:circle(cx2, cy2, 6.5, CAP, { fill = true })
  cv:circle(m2x, m2y, 3, CAP, { fill = true })
  -- ear, cheek, snout
  local ex, ey = P(-11, ear_up and -10 or -6)
  cv:circle(ex, ey, 8, EAR, { fill = true })
  local sx, sy = P(9, 3)
  cv:circle(sx, sy, 7.5, SNOUT, { fill = true })
  local nx, ny = P(15.5, 0.5)
  cv:circle(nx, ny, 3, INK, { fill = true })
  local ix, iy = P(6, -5)
  cv:circle(ix, iy, 2.8, INK, { fill = true })
  if a > 0.5 then   -- tongue out on the way down
    local tx, ty = P(11, 8)
    cv:circle(tx, ty, 2.5, TONGUE, { fill = true })
  end
end

local function draw_pipe(cv, p)
  -- top segment, lip at its bottom edge
  cv:rect(p.x, 0, PW, p.gt, PIPE, { fill = true })
  cv:rect(p.x - 3, p.gt - 24, PW + 6, 24, PIPE, { fill = true })
  cv:line(p.x - 3, p.gt - 1, p.x + PW + 3, p.gt - 1, PIPE_DK, 2)
  -- bottom segment, lip at its top edge
  cv:rect(p.x, p.gb, PW, GROUND_Y - p.gb, PIPE, { fill = true })
  cv:rect(p.x - 3, p.gb, PW + 6, 24, PIPE, { fill = true })
  cv:line(p.x - 3, p.gb + 1, p.x + PW + 3, p.gb + 1, PIPE_DK, 2)
end

local function cover_pipe(cv, p, ox)
  cv:rect(ox - 4, 0, PW + 8, p.gt, SKY, { fill = true })
  cv:rect(ox - 4, p.gb, PW + 8, GROUND_Y - p.gb, SKY,
          { fill = true })
end

local function draw_ground(cv)
  cv:rect(0, GROUND_Y, 400, 400 - GROUND_Y, SAND,
          { fill = true })
  cv:rect(0, GROUND_Y, 400, 11, GRASS, { fill = true })
end

-- The score hugs its digits: the cover is only as
-- wide as the number itself, so it never reads as a
-- banner across the sky.
local function draw_score()
  local s = tostring(score)
  local w = #s * 30 + 20
  c:rect(200 - w / 2, 14, w, 58, SKY, { fill = true })
  c:text(s, 202, 22, SHADOW, 48, "center")
  c:text(s, 200, 20, WHITE, 48, "center")
  drawn_score = score
  score_dirty = false
end

local function draw_play_full()
  c:rect(0, 0, 400, GROUND_Y, SKY, { fill = true })
  draw_ground(c)
  for _, p in ipairs(pipes) do
    draw_pipe(c, p)
    p.px = p.x
  end
  draw_vela(c, BX, by, ang, false)
  pbx, pby = BX, by
  score_dirty = true
  draw_score()
end

local function draw_ready_full()
  c:rect(0, 0, 400, GROUND_Y, SKY, { fill = true })
  draw_ground(c)
  c:text("FLAPPY VELA", 202, 80, SHADOW, 28, "center")
  c:text("FLAPPY VELA", 200, 78, WHITE, 28, "center")
  draw_vela(c, 200, 180, 0, true)
  pbx, pby = 200, 180
  c:text("TAP TO FLAP", 200, 252, WHITE, 20, "center")
  if best > 0 then
    c:text("BEST " .. best, 200, 288, WHITE, 16, "center")
  end
end

local function draw_over_full()
  c:rect(0, 0, 400, GROUND_Y, SKY, { fill = true })
  draw_ground(c)
  c:text("GAME OVER", 202, 82, SHADOW, 28, "center")
  c:text("GAME OVER", 200, 80, WHITE, 28, "center")
  local s = tostring(score)
  local w = #s * 30 + 20
  c:rect(200 - w / 2, 118, w, 58, SKY, { fill = true })
  c:text(s, 202, 126, SHADOW, 48, "center")
  c:text(s, 200, 124, WHITE, 48, "center")
  c:text("BEST " .. best, 200, 198, WHITE, 16, "center")
  draw_vela(c, 200, GROUND_Y - BR + 2, 1.25, false)
  c:text("TAP TO PLAY AGAIN", 200, 246, WHITE, 20, "center")
  pbx, pby = nil, nil
end

-- ---------- game flow ----------
local function spawn_pipe(x)
  local gc = math.random(112, 244)
  pipes[#pipes + 1] = {
    x = x, gt = gc - GAP / 2, gb = gc + GAP / 2,
    passed = false,
  }
end

local function do_flap()
  vy = FLAP
  sound.effect("jump", { volume = 25 })
end

local function start_game()
  score = 0
  by, vy, ang = 190, 0, 0
  pipes = {}
  spawn_pipe(430)
  state = "play"
  draw_play_full()
  vy = FLAP
  sound.effect("jump", { volume = 25 })
end

local function land()
  by = GROUND_Y - BR
  state = "over"
  over_t = 0
  if score > best then
    best = score
    store.set("best", best)
  end
  sound.effect("lose", { volume = 40 })
  draw_over_full()
end

local function hit_pipe()
  state = "fall"
  vy = math.min(vy, -240)
  sound.effect("hit", { volume = 60 })
end

-- ---------- update ----------
local function update_play(dt)
  vy = math.min(vy + GRAV * dt, MAXFALL)
  by = by + vy * dt
  local target = vy < 0 and -0.35
                 or math.min(1.15, vy / 500)
  ang = ang + (target - ang) * math.min(1, dt * 10)
  if by < BR then
    by = BR
    vy = math.max(vy, 0)
  end
  local sp = speed()
  for _, p in ipairs(pipes) do
    p.x = p.x - sp * dt
    if p.x + PW < -6 then p.dead = true end
    if not p.passed and p.x + PW < BX - BR then
      p.passed = true
      score = score + 1
      sound.effect("coin", { volume = 35 })
    end
  end
  local last = pipes[#pipes]
  if not last then
    spawn_pipe(430)
  elseif last.x <= 430 - SPACING then
    spawn_pipe(last.x + SPACING)
  end
  -- collisions
  for _, p in ipairs(pipes) do
    if not p.dead
       and BX + BR - 3 > p.x and BX - BR + 3 < p.x + PW then
      if by - BR + 3 < p.gt or by + BR - 3 > p.gb then
        hit_pipe()
        return
      end
    end
  end
  if by + BR >= GROUND_Y then land() end
end

local function update_fall(dt)
  vy = math.min(vy + GRAV * dt, MAXFALL)
  by = by + vy * dt
  ang = ang + (1.3 - ang) * math.min(1, dt * 7)
  if by + BR >= GROUND_Y then land() end
end

-- ---------- per-frame paint (partial, cover-based) ----------
local function draw_frame()
  for i = #pipes, 1, -1 do
    local p = pipes[i]
    if p.px and (p.dead or p.px ~= p.x) then
      cover_pipe(c, p, p.px)
      if p.px - 4 < 262 and p.px + PW + 4 > 138 then
        score_dirty = true
      end
      p.px = nil
    end
    if p.dead then
      table.remove(pipes, i)
    elseif p.px ~= p.x then
      draw_pipe(c, p)
      if p.x - 4 < 262 and p.x + PW + 4 > 138 then
        score_dirty = true
      end
      p.px = p.x
    end
  end
  -- Vela: cover her old spot (repairing the ground if
  -- the spot reached into it), then draw her new
  if pbx then
    c:rect(pbx - 36, pby - 32, 72, 58, SKY, { fill = true })
    if pby + 26 > GROUND_Y then
      c:rect(pbx - 36, GROUND_Y, 72, 400 - GROUND_Y, SAND,
             { fill = true })
      c:rect(pbx - 36, GROUND_Y, 72, 11, GRASS,
             { fill = true })
    end
    if pbx + 36 > 138 and pbx - 36 < 262 and pby - 32 < 72 then
      score_dirty = true
    end
  end
  draw_vela(c, BX, by, ang, vy < -40)
  pbx, pby = BX, by
  if score_dirty or drawn_score ~= score then draw_score() end
end

local function draw_ready_frame()
  local nb = 180 + math.sin(t_now * 2.2) * 8
  if pbx then
    c:rect(pbx - 36, pby - 32, 72, 58, SKY, { fill = true })
  end
  draw_vela(c, 200, nb, math.sin(t_now * 2.2 + 0.6) * 0.12,
            math.sin(t_now * 6) > 0)
  pbx, pby = 200, nb
end

-- ---------- input ----------
local function handle_touch(kind, x, y)
  if kind ~= "down" then return end
  if state == "ready" then
    start_game()
  elseif state == "play" then
    do_flap()
  elseif state == "over" and over_t > 0.6 then
    start_game()
  end
end

-- launcher icon: Vela and her propeller, aiming at a gap
do
  local ic = ui.canvas{ w = 96, h = 96, color = SKY }
  ic:rect(70, 0, 26, 30, PIPE, { fill = true })
  ic:rect(70, 66, 26, 30, PIPE, { fill = true })
  prop_a = 0.5
  draw_vela(ic, 38, 54, -0.15, true)
  app.icon(ic)
  ic:delete()
end

ui.background(SKY)

c = ui.canvas{
  w = 400, h = 400, color = SKY,
  on_touch = function(kind, x, y) handle_touch(kind, x, y) end,
}
c:partial(true)

draw_ready_full()

local last_t = time.now()
ui.every(16, function()
  local now = time.now()
  local dt = now - last_t
  last_t = now
  if dt > 0.05 then dt = 0.05 end
  t_now = t_now + dt
  prop_a = prop_a + dt * (vy < -40 and 46 or 9)
  if state == "ready" then
    draw_ready_frame()
  elseif state == "play" then
    update_play(dt)
    if state == "play" or state == "fall" then draw_frame() end
  elseif state == "fall" then
    update_fall(dt)
    if state == "fall" then draw_frame() end
  elseif state == "over" then
    over_t = over_t + dt
  end
end)

print("flappy ready, best " .. best)
