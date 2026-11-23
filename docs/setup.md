# Setting up your ESP32-S3

This guide takes a Waveshare ESP32-S3-Touch-AMOLED-1.75C from its factory
firmware to Muse running with the Watch, Stocks and Wind apps and the Lua app
launcher. Expect about an hour, most of it downloads and the first build.

Everything is installed inside this folder (`toolchain/`, `muse-gadget-sdk/`), so
nothing global changes, and deleting the folder removes it all.

## What you need

- **The board:** Waveshare ESP32-S3-Touch-AMOLED-1.75C (ESP32-S3R8, 32 MB flash,
  8 MB PSRAM, 466x466 round AMOLED, microphones and speaker). Other boards
  in the SDK's list may work with a different build profile, but only this one
  is tested.
- **A USB-C data cable** (some cables carry only power).
- **A Mac** with the Xcode command line tools (`xcode-select --install`), Python
  3.10 or newer, git, and about 6 GB free. The scripts use macOS `sed` and serial
  port names. Linux needs small changes.
- **The Muse app** on your phone, with **Developer mode** on (Settings > Devices).
- **An SDK token** from [gadgets.muse.ai](https://gadgets.muse.ai/settings/sdk-tokens)
  (Account > SDK tokens). Review the [Gadget SDK Terms](https://gadgets.muse.ai/sdk-terms).
  Every gadget needs a token to pair. Keep it private: the scripts never write it to disk.
- **A 2.4 GHz WPA2 (or WPA2/WPA3) Wi-Fi network.** WPA1-only networks are refused
  (see Troubleshooting). A phone hotspot works fine.

## 1. Install the toolchain and the SDK

```sh
git clone https://github.com/dayvson/muse-charm-apps.git muse-charm
cd muse-charm
scripts/00_setup.sh
```

This script:

1. Clones the [Muse Gadget SDK](https://github.com/facebookincubator/muse-gadget-sdk)
   into `muse-gadget-sdk/` at commit `b11908c`, the one our patches were made against.
2. Applies [`patches/0001`](../patches/) (a memory fix for live voice) and
   `patches/0002` (the hooks for the apps: tiles, commands, sound), then links
   `firmware/` into the SDK as `esp32/components/muse/apps`.
3. Clones ESP-IDF v6.0.3 into `toolchain/esp-idf-v6.0.3` and installs its ESP32-S3
   tools into `toolchain/espressif`.
4. Creates `toolchain/tools-venv` with esptool, pyserial, Pillow and imageio-ffmpeg.

It's safe to run again: finished steps are skipped.

> Behind a TLS-inspecting corporate proxy, pip and ESP-IDF's downloads fail
> certificate checks. Point `MUSE_CA_BUNDLE` at your proxy's CA bundle first
> (`export MUSE_CA_BUNDLE=/path/to/ca-bundle.pem`, e.g. in `~/.zshrc`).

## 2. Connect the board and back it up

Plug the board in. It shows up as `/dev/cu.usbmodem*`, and the scripts pick the
first one. Set `MUSE_PORT=/dev/cu.usbmodemXXXX` if you have several.

```sh
scripts/01_backup_factory.sh
```

This reads the entire 32 MB flash, verifies the dump against the chip, and saves
`backups/factory-<time>.bin` with its SHA-256. It only reads from the board.
**Don't skip it:** it's your only way back to the Waveshare factory firmware, and
step 5 refuses to erase without it. Restore later with:

```sh
toolchain/tools-venv/bin/esptool --port "$MUSE_PORT" write-flash 0 backups/factory-<time>.bin
```

If the port doesn't appear or the backup can't connect, unplug the board, hold
**BOOT** while plugging it back in (download mode), and try again.

## 3. Choose the avatar

The firmware shows either the SDK's default character or your own Muse's avatar,
an animation made from the clips Muse exports. You can switch at any time.

```sh
scripts/05_avatar.sh default        # the SDK's default avatar (fine for a first build)
```

To make your own (it takes a few minutes, plus a chat with Muse), see
[avatar.md](avatar.md). `scripts/05_avatar.sh status` shows which one the next
build uses.

## 4. Make it yours (optional)

Some defaults are ours. Before building you can change:

- **[`config/device-guide.md`](../config/device-guide.md):** what Muse reads about
  this gadget, including where you live and which zones the watch shows. Keep it
  under 4096 bytes.
- **The watch's zones, the stock symbols and the kite spots:** see
  [native-apps.md](native-apps.md#customising). Muse can also change the watch zones
  and the third kite spot at any time without a rebuild.

## 5. Build

```sh
scripts/02_build.sh
```

This merges [`config/muse-charm.sdkconfig`](../config/muse-charm.sdkconfig) into the
SDK's settings for the 1.75C (`board.sh build s3`) and builds into
`muse-gadget-sdk/esp32/build-muse-waveshare-s3-175c/`. That overlay turns on the
apps (`CONFIG_MUSE_APPS`), the custom partition table (with `luapps`, the Lua
apps' storage, and `avatar`, your avatar's frames), the device guide, and the memory settings described in
[memory.md](memory.md). The first build takes 5 to 15 minutes. It ends by listing
the effective settings and the size of `muse-gadget.bin` (about 3 MB).

## 6. Flash

The first time, the flash has to be erased: Muse keeps its settings where the
factory firmware kept its data.

```sh
scripts/04_flash_muse.sh            # checks the backup, asks you to type ERASE, erases, flashes
```

From then on, reflash new builds without erasing. This keeps pairing and Wi-Fi:

```sh
scripts/04_flash_muse.sh --reflash
```

With your own avatar, its frames go to the `avatar` partition in the same flash,
but only when they changed since the last one (`--avatar` rewrites them anyway).
If the board shows an empty stage instead of your avatar, the partition holds
other frames: flash with `--avatar`.

## 7. Store your SDK token

```sh
scripts/03_set_token.sh
```

Paste the `mgst_…` token when asked. Input is hidden and the token isn't saved
anywhere on the Mac. The board stores it, answers `@sdk_token ok` and restarts.

## 8. Pair with Muse

1. In the Muse app: Settings > Devices (Developer mode on), add a device, and pick
   the one named `MuseGadget-XXXXXX`.
2. Confirm on the board when it asks: follow the on-screen prompt.
3. Give it your Wi-Fi network in the app.

Watch the board's log while it pairs and connects:

```sh
scripts/capture_log.sh 120          # streams and saves logs/serial-<time>.log; tokens and passwords redacted
```

## 9. Use it

- **Tap the PWR button:** a live voice call with Muse. The avatar listens and speaks.
- **Hold PWR:** record a voice note, sent when you release. Replies show as captions.
- **Swipe right from Muse:** Watch, then Stocks, then Wind. **Swipe left:** Apps, then Settings.
- **Ask Muse:** "set my watch to Tokyo time", "show META over a year",
  "how's the wind at Stiltsville tomorrow?". It uses the apps' commands
  ([native-apps.md](native-apps.md)).
- **Lua apps:** send Muse [`muse-prompts/lua-apps.md`](../muse-prompts/lua-apps.md)
  once (in the Muse app, or `scripts/06_ask_muse.sh --file muse-prompts/lua-apps.md`),
  then ask for apps: "build me a pomodoro timer for my gadget". To install the
  ones in [`apps/`](../apps/), give Muse the source and ask it to install it
  ([launcher.md](launcher.md)).

`scripts/06_ask_muse.sh "message"` types a message to Muse through the board and
saves the reply in `logs/`, which is handy for scripting.

`scripts/07_screenshots.sh` opens each installed Lua app over USB and saves a
screenshot of it in `docs/images/apps/` (keep the board awake and on Wi-Fi so apps
can fetch their data). To have Muse commit the apps it builds to your copy of this
repository, send it [`muse-prompts/publish-apps.md`](../muse-prompts/publish-apps.md)
once ([launcher.md](launcher.md)).

## Updating

- **Lua apps** never need a rebuild: Muse reinstalls them over the air.
- **Firmware changes** (native apps, launcher, settings, avatar): `scripts/02_build.sh`
  then `scripts/04_flash_muse.sh --reflash`.
- **A newer SDK:** update `muse-gadget-sdk/`, check the patches still apply
  ([patches/README.md](../patches/README.md)), and rebuild. Patch 0002 touches the
  SDK's UI, Kconfig and command handling, so expect to fix it up now and then.

## Troubleshooting

| Symptom | Fix |
|---|---|
| No `/dev/cu.usbmodem*` | Try another cable (data, not charge-only) or port; enter download mode (hold BOOT while plugging in). |
| `scripts/04_flash_muse.sh` won't erase | It needs a verified 32 MB backup in `backups/`: run `01_backup_factory.sh`. |
| Wi-Fi fails with reason 211 in the log | The network advertises WPA1 only. Switch the router to WPA2 (or WPA2/WPA3 mixed), or use a phone hotspot. |
| Pairing fails or Muse can't reach the board after a token change | Muse records the token when you pair. Hold the button 5 s to reset pairing (or Settings > Reset pairing), then pair again. |
| Voice-note replies are text, not audio | By SDK design: voice notes get captions, calls get voice. |
| Build fails in pip or the component manager with certificate errors | Set `MUSE_CA_BUNDLE` (step 1). |
| Build says "The SDK isn't set up" | Run `scripts/00_setup.sh` (the link to `firmware/` or the SDK is missing). |
| Stocks or Wind stay empty | Fetches wait until Muse's connection is steady, the screen is awake and no call is active. Check `scripts/capture_log.sh` for `apps_net:` lines; low internal RAM skips a fetch ([memory.md](memory.md)). |
| A Lua app hangs the screen | Long-press to exit. Muse can `luapp.admin` `stop`, or with `force=true` reboot the board ([launcher.md](launcher.md)). |
| Muse says it sent a command, but nothing happens on the board | Run `scripts/08_link_check.sh` and ask Muse for a command while it listens: it reports whether the command reached the board and what the link did. `link.register held for DMA headroom` in the log means internal RAM is short ([memory.md](memory.md)). |
| `scripts/07_screenshots.sh` says "no answer" or "screenshots are off" | The firmware predates the apps console or snapshots: rebuild and `scripts/04_flash_muse.sh --reflash`. |
| `scripts/06_ask_muse.sh` returns an empty reply | Muse took longer than the board waits. The reply still arrives in the Muse app. |
| Want the factory firmware back | Write your backup (step 2). |
