# Timer

A countdown timer for the Muse Charm.

## What it does

- Pick a duration from the three preset chips (2, 3 or 5 minutes)
  or set any number of minutes or seconds.
- Start, pause, resume and stop.
- A ring around the rim of the round screen shows the time left. It
  starts full and is eaten away as the timer runs, one new frame
  every second.
- The ring is green, turns yellow inside the last minute (or the last
  third of a short timer), and red for the last seconds.
- At zero the whole screen blinks red with TIME UP.

## Sound

The device has a speaker: Muse's voice plays through it on calls.
But the firmware's command list exposes no way to play a sound: the
commands are the display, the watch and the avatar, and that is the
whole surface. So for now the alarm is the blinking red screen. The
timer already prints a `{"state": "done"}` event at zero; the moment
the device exposes a sound command, the driver plays the alarm on
that event and nothing in this app has to change.

## A page of its own

The device's swipe pages are defined by the firmware: Muse, and the
watch one swipe right. There is no command to register another page,
so the timer cannot yet sit beside the watch as a permanent page; a
drawn frame is an overlay that a tap dismisses. The app is built as
a self-contained page so it can be registered the moment the
firmware allows it.

## Driving the device

Frames go to the gadget with `display.draw_url` today: on the
firmware paired here, `display.draw_file` answers "failed to
allocate" for every image, even a 1.5 KB test file, while the same
JPEG over a URL draws fine (verified 2026-11-21 with a full 30 second
demo run, 20 frames). If a later firmware fixes draw_file, the driver
can switch back with no app changes.

## Touch

The screen shows the preset chips and the start and pause states as
they would look on the device. This firmware sends no touch events
back, so choosing a preset and starting or pausing happens through
the driver (ask Muse, or use the CLI below).

## Run it

```sh
python3 timer.py --preset 5m
python3 timer.py --minutes 3
python3 timer.py --seconds 30
```

Frames land in `frames/`, one JPEG per second, and state changes are
printed as JSON lines on stdout. Send SIGINT to pause or resume and
SIGTERM to stop.
