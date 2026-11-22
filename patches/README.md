# Local SDK changes

scripts/00_setup.sh applies these to muse-gadget-sdk/ (at the SDK commit it pins)
and links firmware/ in. By hand, from muse-gadget-sdk/:

    git apply ../patches/0001-muse_vm-task-stack-via-helper_task.patch
    git apply ../patches/0002-muse-apps-hooks.patch
    ln -sfn ../../../../firmware esp32/components/muse/apps

After updating the SDK, check they still apply (git apply --check) and regenerate
them with git diff if you had to fix anything.

- 0001: muse_vm lookup task via helper_task_create (PSRAM stack) so live-voice builds reach the VM.
- 0002: apps beside Muse (CONFIG_MUSE_APPS): Kconfig option, CMake sources, the
  Watch | Muse | Settings tiles in muse_ui.c, and the watch.*/app.show commands
  in noise_control.cpp (link.register) and app.c (dispatch), and the clock
  fallback in muse_chat_session.cpp (VM replies' Date header -> muse_apps_clock_hint),
  and Lua apps' sound in muse_voice.c's idle loop (muse_apps_sound_pull). It
  applies after 0001 (both touch app.c).
- unused-ptt-calls-for-all-boards.patch: reverted experiment, kept for reference.
