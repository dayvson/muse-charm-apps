# muse-charm-apps

Personalized apps for the Muse Charm connected device, built one at a
time and committed here as they grow.

The Charm's screen is round (466x466), so every app here designs for
the circle: the important content lives in the middle, and the rim is
free for progress rings and other edge displays.

## Apps

- [Timer](apps/timer/) — a countdown timer with preset chips, a ring
  around the rim that is eaten away as time passes, and colours that
  move from green to yellow to red at the end.

## How an app runs

Each app renders its screens as JPEG frames and a driver pushes them
to the device with the gadget's `display.draw_file` command. Apps
print their state changes as JSON lines so the driver can react, for
example by playing a sound or restoring the avatar when an app ends.
