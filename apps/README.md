# Apps by Muse

Lua apps that Muse wrote for the Muse Charm and deployed to the board without a
firmware update, built one at a time and committed here as they grew. Each folder
has the app's Lua source and a README describing what it does, its sound and its
icon, and the id, name and colour Muse installs it with.

The Charm's screen is round (466x466), so every app here designs for the circle:
the important content lives in the middle, and the rim is free for progress rings
and other edge displays.

| App | id | What it is |
|---|---|---|
| [Timer](timer/) | `timer` | A countdown timer with plus and minus buttons, a ring around the rim that is eaten away as time passes, and colours that move from green to yellow to red at the end. |
| [Paddle Ball](paddle/) | `paddle` | An original brick-breaking game on a canvas: drag to move the paddle, tap to launch, clear 35 bricks, with sound effects and a stored best score. |
| [Moon Phase](moon/) | `moon` | A picture of today's moon drawn on a canvas: the phase computed from the date, with the phase name, percent lit, moon age and the next full or new moon. |
| [Horoscope](horoscope/) | `horoscope` | A zodiac wheel for the round screen: swipe and the wheel turns your sign to the top, with a medallion and the day's message in the middle. |
| [Converter](fx/) | `fx` | USD, BRL, EUR, GBP and RUB with each side switchable, a keypad for the amount, and live rates from free keyless sources, cached for offline use. |
| [F1](f1/) | `f1` | Formula 1: the next race with a lapping car on a drawn circuit outline and a countdown ring at the edge of the screen, the weekend schedule, the last race's podium and the championship top 3, from a free keyless API. |
| [Weather](weather/) | `weather` | A living sky for one city at a time (Miami, New York, Rio de Janeiro, San Francisco; tap the city name to switch): a gradient sky with an animated sun, moon, stars, clouds, rain or snow, a big temperature and a glass panel with the next three hours, from Open-Meteo. |

## How an app runs

Each app is a Lua program that runs on the device itself, in the Apps launcher
one swipe left of Muse. Muse installs it with `luapp.install`. The device
compiles, saves and opens it, and it stays in the launcher until removed. Apps
draw with the gadget's own widgets and canvas, keep state with its store, play
sounds on its synth, and exit on a long-press.

How the launcher works, the API apps can use, and Muse's commands:
[docs/launcher.md](../docs/launcher.md).

## Installing one of these on your board

Your Muse needs to know about the launcher first: send it
[`muse-prompts/lua-apps.md`](../muse-prompts/lua-apps.md) once. Then give it the
app's source and ask it to install it, for example:

> Install this Lua app on my gadget with luapp.install: id `timer`, name "Timer",
> colour 0x34C759. Send it in pieces if it's over 8000 characters.

Reinstalling with the same id updates the app.

## Adding an app

Ask Muse to build it ("build me a ... app for my gadget"). When you're happy with
it, save it here as `apps/<id>/<id>.lua` with a README in the same style: what it
does, sound, icon, and how it's installed. Muse can read an installed app's
source back with `luapp.admin` action `source`.
