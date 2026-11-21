-- Horoscope: the 12 signs on one screen; tap yours for the day's
-- message. The message is picked deterministically from the sign
-- and the date, so it is the same all day and changes overnight.
-- Your sign is remembered and opens first next time.

local BG = 0x0B0B12
local PALE = 0xD8D5CC
local LAV = 0x8E8CA8
local GOLD = 0xD9A441
local BTN = 0x23232E
local BTN_SEL = 0x44506B

local SIGNS = {
  {"Aries", "Mar 21 - Apr 19"}, {"Taurus", "Apr 20 - May 20"},
  {"Gemini", "May 21 - Jun 20"}, {"Cancer", "Jun 21 - Jul 22"},
  {"Leo", "Jul 23 - Aug 22"}, {"Virgo", "Aug 23 - Sep 22"},
  {"Libra", "Sep 23 - Oct 22"}, {"Scorpio", "Oct 23 - Nov 21"},
  {"Sagittarius", "Nov 22 - Dec 21"}, {"Capricorn", "Dec 22 - Jan 19"},
  {"Aquarius", "Jan 20 - Feb 18"}, {"Pisces", "Feb 19 - Mar 20"},
}

local OPENERS = {
  "The morning starts slow, but it is setting up something good.",
  "A small conversation today carries more weight than it seems.",
  "Today rewards the thing you have been putting off.",
  "Someone is paying closer attention to you than you think.",
  "The plan will change once today, and the change is the upgrade.",
  "Your energy returns in the afternoon; save the hard thing for then.",
  "A familiar place gives you an unfamiliar idea.",
  "Today is better for finishing than for starting.",
  "News you have been waiting on moves a step closer.",
  "A shortcut tempts you; the long way pays better this time.",
  "You are funnier than usual today. Use it.",
  "An old contact resurfaces with something worth hearing.",
}

local MIDDLES = {
  "Keep money decisions simple until tomorrow.",
  "Say the honest thing early; it lands well.",
  "A walk clears the question you keep circling.",
  "Put the phone down for one hour and the day improves.",
  "Help arrives from an unexpected direction; accept it gracefully.",
  "Your work speaks for itself today, so let it.",
  "Leave room in the schedule for one surprise.",
  "Eat the good meal. You have earned it.",
  "Double-check one detail before you send anything important.",
  "A friend needs the short version of you, not the busy one.",
  "The water, the wind or the road will do you good.",
  "Tidy one small corner of your world and your head follows.",
}

local CLOSERS = {
  "By tonight, you will know more than you do now.",
  "End the day with one page, one song or one call.",
  "Luck is quiet today, but it is on your side.",
  "Tomorrow asks less of you. Today asks focus.",
  "Trust the first answer you gave yourself.",
  "A good day to be early, not late.",
  "Keep one promise you made to yourself.",
  "The stars suggest an early night.",
  "Something small you do today echoes next week.",
  "Be generous with credit; it comes back around.",
  "Finish one thing completely before dinner.",
  "Your sign is showing its best side today.",
}

local COLORS = {"teal", "coral", "gold", "sage", "slate", "cream",
                "rust", "sky", "plum", "olive", "copper", "pearl"}
local MOODS = {"bold", "calm", "curious", "playful", "focused",
               "generous", "patient", "bright", "grounded", "daring",
               "gentle", "sharp"}
local MONTHS = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"}

local function pick(pool, sign, salt)
  local day = math.floor(time.now() / 86400) % 10000
  local h = day * 131 + sign * 17 + salt * 101
  return pool[h % #pool + 1]
end

local function lucky_number(sign)
  local day = math.floor(time.now() / 86400) % 10000
  return (day * 7 + sign * 13) % 99 + 1
end

local show_signs -- forward declaration

local function show_day(sign)
  ui.clear()
  local name, dates = SIGNS[sign][1], SIGNS[sign][2]
  local d = time.date()
  local msg = pick(OPENERS, sign, 1) .. " " .. pick(MIDDLES, sign, 2) ..
              " " .. pick(CLOSERS, sign, 3)
  ui.label{text = name, y = -138, size = 28, color = GOLD}
  ui.label{text = dates, y = -102, size = 14, color = LAV}
  ui.label{text = msg, y = -8, size = 16, color = PALE, w = 252,
           align = "center"}
  ui.label{text = "Lucky number: " .. lucky_number(sign), y = 94,
           size = 14, color = PALE}
  ui.label{text = "Colour: " .. pick(COLORS, sign, 4) ..
                  "    Mood: " .. pick(MOODS, sign, 5),
           y = 118, size = 14, color = LAV}
  ui.button{text = "ALL SIGNS", y = 166, w = 132, h = 44, size = 14,
            color = BTN, text_color = PALE, radius = 22,
            on_tap = function() show_signs() end}
  ui.label{text = MONTHS[d.month] .. " " .. d.day .. ", " .. d.year,
           y = -178, size = 14, color = LAV}
  print("horoscope: " .. name .. ", lucky " .. lucky_number(sign))
end

show_signs = function()
  ui.clear()
  local saved = store.get("sign")
  ui.label{text = "HOROSCOPE", y = -180, size = 16, color = LAV}
  local xs = {-112, 0, 112}
  local ys = {-122, -41, 40, 121}
  for i, s in ipairs(SIGNS) do
    local col = (i - 1) % 3 + 1
    local row = math.floor((i - 1) / 3) + 1
    local idx = i
    ui.button{
      text = s[1], x = xs[col], y = ys[row], w = 104, h = 62, size = 14,
      color = (saved == idx) and BTN_SEL or BTN,
      text_color = PALE, radius = 14,
      on_tap = function()
        store.set("sign", idx)
        sound.effect("select", {volume = 40})
        show_day(idx)
      end,
    }
  end
  ui.label{text = "TAP YOUR SIGN", y = 182, size = 14, color = LAV}
end

ui.background(BG)
local saved = store.get("sign")
if saved and SIGNS[saved] then
  show_day(saved)
else
  show_signs()
end
print("horoscope ready")
