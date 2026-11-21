# Converter

A currency converter for USD, BRL, EUR, GBP and RUB, running on
the gadget in the Apps by Muse launcher. Lua source: `fx.lua`.
Installed on the gadget with id `fx`.

## How it works

- Each side switches on its own: tap the top currency code to
  change what you type in, the second code to change what it
  converts to, and the <-> button between them to swap the two
  sides outright.
- Amounts are typed on the keypad (C clears). The converted
  amount and the unit rate update as you type, and the chosen
  pair and amount are kept in the app store for next time.
- Built from widgets rather than a canvas, to keep the gadget's
  memory free for the network fetch.

## Rates

Live rates come from free keyless sources, tried in turn:
open.er-api.com first, then the fawazahmed0 dataset on jsDelivr,
with a few retries at a growing pause, since a fetch made while
the gadget is busy can fail. Fetched rates are cached in the
store, so the app opens instantly and works offline with the
last saved rates; before the first fetch it uses built-in rates
dated 20 Nov 2026. The rate line on screen says which state the
rates are in: live, saved or offline.

## Icon

The app sets its launcher icon at start-up with app.icon: two
swap arrows drawn on a 96x96 canvas over the app's green. The
board keeps it, so the launcher shows the arrows instead of
the app's initials.
