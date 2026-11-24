# Local SDK changes

scripts/00_setup.sh applies these to muse-gadget-sdk/ (at the SDK commit it pins)
and links firmware/ in. By hand, from muse-gadget-sdk/:

    git apply ../patches/0002-muse-apps-hooks.patch
    ln -sfn ../../../../firmware esp32/components/muse/apps

After updating the SDK, check they still apply (git apply --check) and regenerate
them with git diff if you had to fix anything.

- 0001 (removed): the muse_vm lookup task via helper_task_create. The SDK does
  this itself since #397, so it went when we moved to e17081a.
- 0002: apps beside Muse (CONFIG_MUSE_APPS): Kconfig option, CMake sources, the
  Watch | Muse | Settings tiles in muse_ui.c, and the watch.*/app.show commands
  in noise_control.cpp (link.register) and app.c (dispatch), and the clock
  fallback in muse_chat_session.cpp (VM replies' Date header -> muse_apps_clock_hint),
  and Lua apps' sound in muse_voice.c's idle loop (muse_apps_sound_pull), and the
  serial console's "apps COMMAND {json}" line in muse_glue.c (muse_apps_console,
  used by scripts/07_screenshots.sh), and three hooks for Lua apps (luapp_dev.c):
  typed-turn replies in muse_chat_text.c's muse_hatch_console (muse.ask), the
  bottom button's short press in muse_input.c (ui.on_button), and the brightness
  in muse_ui.c (app.brightness). In muse_ui.c it also places the SDK's latest
  reply page (CONFIG_MUSE_LATEST_REPLY) between the app tiles and Muse:
  Wind | Stocks | Watch | Reply | Muse | Apps | Settings, for which
  muse_latest_reply_build takes a column. Our headers are included as
  "apps/muse_apps.h" because the SDK's WebAssembly apps (CONFIG_MUSE_WASM_APPS)
  have their own muse_apps.h, and CONFIG_MUSE_APPS can't be on with them (three
  function names clash). Until e17081a it also lowered the link's DMA margin
  to 12 KB; the SDK's own RAM changes for this board replaced that.
- unused-ptt-calls-for-all-boards.patch: reverted experiment, kept for reference.
