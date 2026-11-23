# Engineering notes: memory, and why the patches and settings exist

The ESP32-S3 on this board has about 512 KB of internal SRAM and 8 MB of PSRAM.
Muse's live voice (Wi-Fi, TLS, audio, the Noise session) uses most of the internal
RAM. Nearly every problem we hit came down to memory, so here's what we learned.
It should save the next person some serial logs.

## Internal RAM fragments during live voice

After a voice call, the largest free block of internal RAM can drop to about 7 KB,
with only about 22 KB free in total. Any task asking for an 8 KB internal stack then
fails, often silently. So:

- **`CONFIG_HOMEHUB_SETUP_TASKS_IN_PSRAM=y`:** pairing, Wi-Fi scan and provisioning
  tasks get PSRAM stacks. Without it, provisioning fails with
  `error_operation_in_progress`.
- **`CONFIG_HOMEHUB_TUNNEL` off:** frees about 60 KB of internal RAM. With the tunnel
  up, the voice session's VM lookup couldn't start, and the board showed "CAN'T REACH
  MUSE". The cost is that Muse can't reach home-network devices through this gadget.
- **[`patches/0001`](../patches/0001-muse_vm-task-stack-via-helper_task.patch):**
  creates the `muse_vm` lookup task with the SDK's `helper_task_create`, which can put
  its stack in PSRAM, instead of a fixed 8 KB internal stack.
- **Every task the apps create** (the network fetches, the Lua host, the stop
  watchdog) has its stack in PSRAM.
- **Fetches check first:** [`apps_net.c`](../firmware/apps_net.c) skips a fetch if
  internal RAM is under 16 KB free or the largest block is under 4 KB. mbedTLS
  allocates from PSRAM in this configuration, and lwIP prefers it.
- **Small allocations** (under 4 KB) still go to internal RAM, so the parsers avoid
  building JSON trees (no cJSON for the Stocks and Wind responses).

## Every byte of firmware costs PSRAM

`CONFIG_SPIRAM_XIP_FROM_PSRAM=y` (the SDK's default on S3 boards) copies the whole
firmware image into PSRAM at boot, and runs code and reads constants from there.
That makes flash writes safe while running, so we left it on. The catch is that
the firmware's size comes straight out of free PSRAM.

With a 1 MB avatar and a 3.9 MB image, only about 590 KB of PSRAM was left at run
time. What we did:

- **The avatar's frames moved out of the firmware**, into their own 4 MB `avatar`
  flash partition, read one 64x64 frame at a time (a 4 KB buffer). At first we
  trimmed them to 512 KB by lowering frame rates; now the clips play at their own
  rate (Vela: about 3 MB of frames) and cost no PSRAM
  ([avatar.md](avatar.md)).
- **The watch dial is a 256-colour indexed image** (218 KB) that LVGL draws from flash
  row by row, instead of a 424 KB RGB565 copy in RAM.
- **The stocks chart is an 8-bit coverage map** tinted with the trend colour: 45 KB
  instead of about 180 KB.
- **Chart buffers exist only near the screen:** Stocks and Wind allocate them only
  while their tile is on screen or one swipe away (`muse_apps_near`). A Lua app
  starting frees them at once (`muse_apps_trim`).
- **Screenshots draw in strips:** a whole-screen snapshot needs a 434 KB buffer,
  which a running app with a big canvas leaves no room for. `luapp.admin`
  `screenshot` ([`luapp_shot.c`](../firmware/luapp_shot.c)) draws 32 rows at a time
  into a ~30 KB strip and feeds each one to the ROM's deflate as PNG rows: about
  200 KB at the peak.
- **A 512 KB PSRAM reserve for voice** (`MUSE_APPS_PSRAM_RESERVE`): Lua images,
  canvases and the native apps never take the last 512 KB.
- **LVGL allocates from PSRAM** (`muse_lv_mem.c` in the SDK).

The firmware is now about 2.9 MB (with your own avatar, whose frames live in their partition), in a 4 MB app slot.

## The command list is capped

`link.register`, the list of commands the board sends Muse, is capped at 8 KB, and
the SDK's own commands take about 2.8 KB of it. So the apps' command descriptions
are terse ([`muse_apps.c`](../firmware/muse_apps.c)). The detail goes elsewhere:
the device guide (up to 4096 bytes) for what the apps are for, and `luapp.admin`
`api` (about 4 KB per reply, so split into five actions: `api`, `api_ui`,
`api_gfx`, `api_sound`, `api_device`) for the Lua API.

## The link needs DMA-capable RAM

The SDK holds back `link.register` until a margin of DMA-capable internal RAM is
free (`NOISE_TX_DMA_RESERVE_BYTES`, 16 KB). With live voice on, this board idles
at 13-17 KB, so the list of commands was sometimes never sent and no Muse command
reached the board. Patch 0002 lowers the margin to 12 KB when the apps are on and
the home-network tunnel is off, and `noise_control.cpp` logs `link.register held
for DMA headroom` if it still waits over 10 s. `scripts/08_link_check.sh` checks
this end to end: it runs a command over the serial console, then watches while you
ask Muse for one.

## Sending less to the screen

A full-screen frame takes about 33 ms to send over the display bus, which capped
canvas games well below 60 fps. Two changes:

- **Timers go down to 16 ms** (they were silently rounded up to 50 ms, so every
  game ran at 20 fps).
- **`c:partial(true)`** sends only the areas drawn since the last `c:clear()`, plus
  the previous frame's. At first it had no effect: LVGL's own canvas calls
  (`fill_bg`, `finish_layer`) marked the whole canvas dirty every frame. The canvas
  code now turns LVGL's invalidation off around those calls
  ([`luapp_gfx.c`](../firmware/luapp_gfx.c)). `perf` reports the KB actually sent per
  frame, so it's easy to check.

## Keeping Muse responsive

- **No fetches during calls:** fetches wait until no call is active, the screen is
  awake, and Muse's session has been steady for 3 s. A stock prefetch at boot once
  crowded out pairing.
- **Lua apps can't starve Muse:** the Lua task handles Muse's commands before the
  app's timers. It paces frames (after a round taking T ms, it waits at least
  max(5 ms, T/3)), and a hook aborts any callback that runs past 250 ms. `luapp.admin`
  `stop` with `force=true` reboots the board if an app still won't stop in 3 s, and
  the next boot reports it in `list`.
- **Sound** from Lua apps is mixed in by Muse's voice task only while Muse is idle
  (patch 0002, `muse_voice.c`), so it never competes with a call.

## Useful log lines

`scripts/capture_log.sh` saves the serial log to `logs/` (use `grep -a`: the logs
contain binary bytes).

- `int=22K/7K`-style figures: internal RAM free and its largest block.
- `apps_net: HTTP ...` and `skipped: internal RAM ...`: fetches and why one didn't run.
- `X unloaded (why): freed N KB`: a Lua app stopping, and what it gave back.
