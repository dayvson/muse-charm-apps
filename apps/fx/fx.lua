-- Converter: USD, BRL, EUR, GBP and RUB, with each side
-- switchable on its own. Tap a currency code to change it, the
-- middle button to swap the two sides, and type the amount on
-- the keypad. Built from widgets, not a canvas, so the gadget
-- keeps enough memory free for the rate fetch. Rates come from
-- Frankfurter (BRL/EUR/GBP) and open.er-api.com (RUB only), both
-- free with no key, cached in the store so the app opens
-- instantly and still works offline with the last saved rates.

local BG = 0x0A0F0C
local PALE = 0xD8D5CC
local LAV = 0x8E8CA8
local BRIGHT = 0x7ED491
local PANEL = 0x1C1C26
local DIM = 0x3A3A48
local RED = 0xE5484D

local CURS = {"USD", "BRL", "EUR", "GBP", "RUB"}
-- built-in rates per 1 USD, verified 21 Nov 2026 against
-- Frankfurter (ECB, 20 Nov: BRL 5.938, EUR 0.90992, GBP 0.76192)
-- and the Bank of Russia daily rate (21 Nov: USD 82.1911 RUB).
-- Used until a fetch lands; labelled "base", not "offline".
local rates = {USD = 1, BRL = 5.938, EUR = 0.9099, GBP = 0.7619,
               RUB = 82.19}
local rate_state = "base" -- live, saved, base

local from_i = store.get("from") or 1
local to_i = store.get("to") or 2
local amt = store.get("amt") or ""

-- Store version 2: rates saved by the first build are ignored,
-- so a stale or wrong cached rate cannot override the fix.
do
  if store.get("fxver") == 2 then
    local any = false
    for _, code in ipairs(CURS) do
      local r = store.get("r" .. code)
      if r then rates[code] = r; any = true end
    end
    if any then rate_state = "saved" end
  else
    store.set("fxver", 2)
  end
end

local function fmt(n)
  local s = string.format("%.2f", n)
  local int, frac = s:match("^(%d+)%.(%d+)$")
  local out, cnt = "", 0
  for i = #int, 1, -1 do
    out = int:sub(i, i) .. out
    cnt = cnt + 1
    if cnt == 3 and i > 1 then out = "," .. out; cnt = 0 end
  end
  return out .. "." .. frac
end

local from_btn, to_btn, amt_label, result_label, rate_label

local function refresh()
  local from, to = CURS[from_i], CURS[to_i]
  local a = tonumber(amt) or 0
  local res = a / rates[from] * rates[to]
  from_btn:set{text = from}
  to_btn:set{text = to}
  amt_label:set{text = amt == "" and "0" or amt}
  result_label:set{text = fmt(res)}
  local unit = rates[to] / rates[from]
  rate_label:set{text = "1 " .. from .. " = " ..
                 string.format("%.4f", unit) .. " " .. to ..
                 "  -  " .. rate_state}
end

local function cycle(side)
  if side == "from" then
    from_i = from_i % #CURS + 1
    store.set("from", from_i)
  else
    to_i = to_i % #CURS + 1
    store.set("to", to_i)
  end
  sound.effect("select", {volume = 30})
  refresh()
end

local function press(k)
  if k == "C" then
    amt = ""
  elseif k == "." then
    if not amt:find("%.") then amt = (amt == "" and "0" or amt) .. "." end
  else
    if #amt < 10 then amt = amt .. k end
  end
  store.set("amt", amt)
  sound.effect("click", {volume = 22})
  refresh()
end

-- launcher icon: two swap arrows on the app's green
do
  local ic = ui.canvas{w = 96, h = 96, color = 0x2F9E63}
  ic:line(26, 36, 62, 36, 0xF5F5F0, 7)
  ic:triangle(62, 27, 62, 45, 74, 36, 0xF5F5F0)
  ic:line(70, 60, 34, 60, 0xF5F5F0, 7)
  ic:triangle(34, 51, 34, 69, 22, 60, 0xF5F5F0)
  app.icon(ic)
  ic:delete()
end

ui.background(BG)

from_btn = ui.button{x = -102, y = -168, w = 116, h = 54, size = 28,
  color = PANEL, text_color = BRIGHT, radius = 14,
  on_tap = function() cycle("from") end}
amt_label = ui.label{x = 92, y = -168, w = 176, size = 28,
  color = PALE, align = "right", text = "0"}

ui.line{points = {-168, -124, -36, -124}, color = DIM, width = 2}
ui.line{points = {36, -124, 168, -124}, color = DIM, width = 2}
ui.button{x = 0, y = -124, w = 56, h = 38, text = "<->", size = 16,
  color = PANEL, text_color = BRIGHT, radius = 19,
  on_tap = function()
    from_i, to_i = to_i, from_i
    store.set("from", from_i)
    store.set("to", to_i)
    sound.effect("select", {volume = 30})
    refresh()
  end}

to_btn = ui.button{x = -102, y = -80, w = 116, h = 54, size = 28,
  color = PANEL, text_color = BRIGHT, radius = 14,
  on_tap = function() cycle("to") end}
result_label = ui.label{x = 92, y = -80, w = 176, size = 28,
  color = PALE, align = "right", text = "0.00"}

rate_label = ui.label{x = 0, y = -36, w = 320, size = 14,
  color = LAV, align = "center", text = ""}

local keys = {"1", "2", "3", "4", "5", "6", "7", "8", "9",
              ".", "0", "C"}
for i, k in ipairs(keys) do
  local col = (i - 1) % 3
  local row = math.floor((i - 1) / 3)
  ui.button{x = -76 + col * 76, y = 8 + row * 58, w = 68, h = 50,
    text = k, size = 20, color = PANEL,
    text_color = k == "C" and RED or PALE, radius = 12,
    on_tap = function() press(k) end}
end

refresh()
print("converter ready, " .. CURS[from_i] .. " to " .. CURS[to_i] ..
      ", rates " .. rate_state)

local function apply(new_rates, source)
  for _, code in ipairs(CURS) do
    local r = new_rates[code]
    if r then
      rates[code] = r
      store.set("r" .. code, r)
    end
  end
  rate_state = "live"
  refresh()
  print("rates updated live from " .. source)
end

-- Fetch strategy (fixed 21 Nov 2026): the first build fetched
-- two all-currency files. The jsdelivr usd.json is ~200 KB and
-- the er-api reply carries ~180 rates; on the gadget the fetch
-- never landed, so the app sat on "offline". Now:
-- 1. Frankfurter for BRL/EUR/GBP: symbols-filtered, a ~120-byte
--    reply, free, no key. Frankfurter has no RUB (ECB suspended
--    it), so it cannot be the only source.
-- 2. RUB only, pattern-matched out of the er-api reply as text,
--    so no full JSON decode of the large body is needed.
local function fetch_rub(attempt)
  http.get("https://open.er-api.com/v6/latest/USD",
  function(ok, body)
    if ok and body then
      local rub = body:match('"RUB":([%d%.]+)')
      if rub then
        apply({RUB = tonumber(rub)}, "er-api RUB")
        return
      end
      print("RUB fetch: no RUB rate in reply")
    else
      print("RUB fetch failed: " .. tostring(body))
    end
    if attempt < 3 then
      ui.after(4000 * attempt, function() fetch_rub(attempt + 1) end)
    end
  end)
end

local attempt = 0
local function try_fetch()
  attempt = attempt + 1
  http.get("https://api.frankfurter.dev/v1/latest" ..
           "?base=USD&symbols=BRL,EUR,GBP", function(ok, body)
    if ok and body then
      local data = json.decode(body)
      if data and data.rates then
        apply(data.rates, "frankfurter")
        fetch_rub(1)
        return
      end
      print("fetch try " .. attempt .. ": unreadable reply")
    else
      print("fetch try " .. attempt .. " failed: " .. tostring(body))
    end
    if attempt < 3 then
      ui.after(4000 * attempt, try_fetch)
    else
      fetch_rub(1)
    end
  end)
end
ui.after(1500, try_fetch)
