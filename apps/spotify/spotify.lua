-- Spotify: a remote for the gadget, in Spotify's own
-- colours. Screen 1 shows the playing track with its
-- album art, a progress bar, and Previous / Play-Pause /
-- Next controls under the Spotify logo. Screen 2 is
-- discovery: this week's Release Radar, one new song at
-- a time, tap play to hear it. Swipe or press the bottom
-- button to switch screens.
--
-- The gadget cannot talk to Spotify itself (that needs
-- the account login), so the app asks Muse: muse.ask
-- carries "SPOTIFY ..." commands and Muse answers with a
-- small JSON state, which is also what luapp.send pushes.
-- Album art comes from Spotify's public oEmbed page for
-- the track: its thumbnail URL carries a size code, and
-- swapping 00001e02 for 00004851 gives the 64 px version
-- (about 3 KB, inside the http body cap).

local touch_fn = nil
local c = ui.canvas{
  w = 466, h = 466, color = 0x121212,
  on_touch = function(kind, x, y)
    if touch_fn then touch_fn(kind, x, y) end
  end,
}
c:partial(true)
ui.background(0x121212)

local BG = 0x121212
local CARD = 0x1E1E1E
local GREEN = 0x1DB954
local WHITE = 0xFFFFFF
local GREY = 0xB3B3B3
local TRACK = 0x404040

local st = {playing = false, title = "", artist = "",
            tid = nil, prog = 0, dur = 0}
local screen = 1
local art_img = nil     -- gfx image of the current art
local asked_track_end = false

local disc = {}         -- discovery items
local disc_i = 1
local disc_art = nil
local disc_art_i = nil
local disc_asked = false

-- gadget text is ASCII only: drop high bytes (accents go,
-- the letters around them stay)
local function clean(s)
  if not s then return "" end
  return (s:gsub("[\128-\255]", ""))
end
local function trunc(s, n)
  s = clean(s)
  if #s > n then return s:sub(1, n - 2) .. ".." end
  return s
end

-- the Spotify mark: a green disc with three black arcs
local function draw_logo(cx, cy, r)
  c:circle(cx, cy, r, GREEN, {fill = true})
  for _, rr in ipairs({0.34, 0.55, 0.76}) do
    local rad = r * rr + 2
    local ox = cx - rad * 0.42
    local pts = {}
    for k = -4, 4 do
      local a = math.rad(k * 13)
      pts[#pts + 1] = ox + rad * math.cos(a)
      pts[#pts + 1] = cy + rad * math.sin(a)
    end
    for k = 1, #pts - 3, 2 do
      c:line(pts[k], pts[k + 1], pts[k + 2], pts[k + 3],
             BG, math.max(2, math.floor(r / 6)))
    end
  end
end

local function draw_header(sub)
  draw_logo(58, 46, 19)
  c:text(sub, 160, 34, WHITE, 24, "center")
end

-- ---------- art fetching ----------
local function fetch_art(tid, cb)
  local url = "https://open.spotify.com/oembed?url=" ..
    "https://open.spotify.com/track/" .. tid
  http.get(url, function(ok, body)
    print("art oembed ok=" .. tostring(ok))
    if not ok then cb(nil) return end
    local d = json.decode(body)
    if not d or not d.thumbnail_url then
      print("art oembed: no thumbnail")
      cb(nil) return
    end
    local u = d.thumbnail_url:gsub("00001e02", "00004851")
    http.get(u, function(ok2, img)
      print("art img ok=" .. tostring(ok2) ..
            " len=" .. (ok2 and #img or 0))
      if not ok2 then cb(nil) return end
      local im = gfx.image(img)
      print("art gfx ok=" .. tostring(im ~= nil))
      if im then
        -- keep the picture for next time
        pcall(function() fs.write("art_" .. tid, img) end)
      end
      cb(im)
    end)
  end)
end
local function cached_art(tid)
  local ok, data = pcall(function() return fs.read("art_" .. tid) end)
  if ok and data and #data > 100 then return gfx.image(data) end
  return nil
end

-- ---------- talking to Muse ----------
local function apply_state(d)
  if d.title then st.title = d.title end
  if d.artist then st.artist = d.artist end
  st.note = d.note
  if d.playing ~= nil then st.playing = d.playing end
  if d.prog then st.prog = d.prog end
  if d.dur then st.dur = d.dur end
  if d.tid and d.tid ~= st.tid then
    st.tid = d.tid
    asked_track_end = false
    art_img = cached_art(d.tid)
    if not art_img then
      fetch_art(d.tid, function(im)
        if im and st.tid == d.tid then
          art_img = im
          if screen == 1 then draw_all() end
        end
      end)
    end
  end
  if screen == 1 then draw_all() end
end
local function route_reply(ans)
  local d = json.decode(ans)
  if not d then return end
  if d.t == "state" then
    apply_state(d)
  elseif d.t == "disc" and d.items then
    disc = d.items
    disc_i = 1
    disc_art, disc_art_i = nil, nil
    if screen == 2 then draw_disc() end
  end
end
local function ask(cmd)
  muse.ask(cmd, function(ans, err)
    if ans then route_reply(ans) end
  end)
end

-- ---------- shared pieces ----------
local function draw_art_big(img)
  c:rect(131, 78, 204, 204, CARD, {radius = 10})
  if img then
    -- canvas image scale is a PERCENT: 300 means 3x
    c:image(img, 137, 84, {scale = 300})
  else
    draw_logo(233, 180, 34)
  end
end
local function mmss(s)
  if s < 0 then s = 0 end
  return string.format("%d:%02d",
    math.floor(s / 60), math.floor(s % 60))
end
local function draw_progress()
  c:rect(83, 352, 300, 6, TRACK, {radius = 3})
  if st.dur > 0 and st.prog > 0 then
    local w = math.floor(300 * st.prog / st.dur)
    if w > 300 then w = 300 end
    if w > 6 then c:rect(83, 352, w, 6, GREEN, {radius = 3}) end
  end
  c:text(mmss(st.prog), 75, 349, GREY, 12, "right")
  c:text(mmss(st.dur), 391, 349, GREY, 12, "left")
end
local function skip_btn(cx, cy, dir)
  c:circle(cx, cy, 26, CARD, {fill = true})
  if dir < 0 then
    -- previous: bar on the left, two LEFT-pointing triangles
    c:rect(cx - 14, cy - 11, 4, 22, WHITE)
    c:triangle(cx - 2, cy - 11, cx - 2, cy + 11, cx - 12, cy, WHITE)
    c:triangle(cx + 10, cy - 11, cx + 10, cy + 11, cx + 1, cy, WHITE)
  else
    -- next: two RIGHT-pointing triangles, bar on the right
    c:triangle(cx - 10, cy - 11, cx - 10, cy + 11, cx - 1, cy, WHITE)
    c:triangle(cx + 2, cy - 11, cx + 2, cy + 11, cx + 12, cy, WHITE)
    c:rect(cx + 10, cy - 11, 4, 22, WHITE)
  end
end
local function play_btn(cx, cy, r, playing)
  c:circle(cx, cy, r, GREEN, {fill = true})
  if playing then
    c:rect(cx - 9, cy - 11, 7, 22, BG)
    c:rect(cx + 2, cy - 11, 7, 22, BG)
  else
    c:triangle(cx - 7, cy - 12, cx - 7, cy + 12, cx + 13, cy, BG)
  end
end

-- ---------- now playing (screen 1) ----------
function draw_all()
  c:clear(BG)
  draw_header("Spotify")
  draw_art_big(art_img)
  c:text(trunc(st.title, 24), 233, 300, WHITE, 24, "center")
  c:text(trunc(st.artist, 32), 233, 330, GREY, 16, "center")
  draw_progress()
  skip_btn(120, 408, -1)
  play_btn(233, 406, 32, st.playing)
  skip_btn(346, 408, 1)
  if st.note then
    c:rect(63, 158, 340, 44, 0x000000, {opacity = 210, radius = 22})
    c:text(st.note, 233, 169, WHITE, 15, "center")
  end
end

-- ---------- discovery (screen 2) ----------
function draw_disc()
  c:clear(BG)
  draw_header("Release Radar")
  local it = disc[disc_i]
  if not it then
    c:text(disc_asked and "Nothing back yet" or "Asking Muse..",
           233, 220, GREY, 16, "center")
    return
  end
  if disc_art_i ~= disc_i then
    disc_art_i = disc_i
    disc_art = cached_art(it.tid)
    if not disc_art then
      local mine = disc_i
      fetch_art(it.tid, function(im)
        if im and disc_i == mine then
          disc_art = im
          if screen == 2 then draw_disc() end
        end
      end)
    end
  end
  draw_art_big(disc_art)
  c:text(trunc(it.name, 24), 233, 300, WHITE, 20, "center")
  c:text(trunc(it.artist, 32), 233, 330, GREY, 15, "center")
  c:text(disc_i .. " / " .. #disc, 233, 356, GREY, 13, "center")
  skip_btn(120, 408, -1)
  play_btn(233, 406, 32, false)
  skip_btn(346, 408, 1)
end

local function show(n)
  screen = n
  if n == 1 then
    draw_all()
  else
    if not disc_asked then
      disc_asked = true
      ask("SPOTIFY DISCOVER")
    end
    draw_disc()
  end
end

-- ---------- input ----------
touch_fn = function(kind, x, y)
  if kind ~= "up" then return end
  sound.effect("click", {volume = 20})
  if screen == 1 then
    if y > 372 then
      if x < 176 then ask("SPOTIFY PREV")
      elseif x > 290 then ask("SPOTIFY NEXT")
      elseif st.playing then ask("SPOTIFY PAUSE")
      else ask("SPOTIFY PLAY") end
    elseif y > 70 and y < 290 then
      if st.playing then ask("SPOTIFY PAUSE")
      else ask("SPOTIFY PLAY") end
    end
  else
    local it = disc[disc_i]
    if y > 372 then
      if x < 176 then
        if disc_i > 1 then disc_i = disc_i - 1 draw_disc() end
      elseif x > 290 then
        if disc_i < #disc then disc_i = disc_i + 1 draw_disc() end
      elseif it then
        ask("SPOTIFY PLAYURI spotify:track:" .. it.tid)
        show(1)
      end
    elseif it and y > 70 and y < 290 then
      ask("SPOTIFY PLAYURI spotify:track:" .. it.tid)
      show(1)
    end
  end
end
ui.on_swipe(function(dir) show(screen == 1 and 2 or 1) end)
ui.on_button(function() show(screen == 1 and 2 or 1) end)

app.on_message = function(data)
  route_reply(data)
end

-- progress, once a second: repaint just the bar's strip
ui.every(1000, function()
  if screen ~= 1 or not st.playing then return end
  st.prog = st.prog + 1
  c:rect(35, 344, 396, 20, BG)
  draw_progress()
  if st.dur > 0 and st.prog > st.dur + 2
     and not asked_track_end then
    asked_track_end = true
    ask("SPOTIFY STATE")
  end
end)

-- launcher icon: the Spotify mark on black
do
  local ic = ui.canvas{w = 96, h = 96, color = 0x121212}
  ic:circle(48, 48, 30, GREEN, {fill = true})
  for _, rr in ipairs({10, 16, 22}) do
    local ox = 48 - rr * 0.42
    for k = -4, 3 do
      local a1 = math.rad(k * 13)
      local a2 = math.rad((k + 1) * 13)
      ic:line(ox + rr * math.cos(a1), 48 + rr * math.sin(a1),
              ox + rr * math.cos(a2), 48 + rr * math.sin(a2),
              0x121212, 4)
    end
  end
  app.icon(ic)
  ic:delete()
end

draw_all()
print("spotify ready")
ui.after(700, function() ask("SPOTIFY STATE") end)
