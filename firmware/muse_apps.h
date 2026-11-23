#pragma once

/*
 * Apps beside Muse (CONFIG_MUSE_APPS): tiles one swipe right of the avatar,
 * which Muse controls through link commands. This header is what Link's
 * session code needs: advertise the commands and answer them.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Adds the apps' commands to link.register's `commands` object. */
void muse_apps_register_commands(cJSON *commands);

/* True for a command muse_apps_command() answers. */
bool muse_apps_handles(const char *command);

/* Runs on the Noise session task and doesn't block. Returns {"ok": true,
 * "payload": ...}, {"ok": false, "error": {code, message}}, or {"_async":
 * true} when the result comes later through the reply function. */
cJSON *muse_apps_command(const char *command, const cJSON *params, const char *request_id, uint64_t generation);

/* The serial console's apps command, for the bench and screenshots
 * (scripts/07_screenshots.sh): a line "apps COMMAND [JSON params]", such as
 * apps luapp.run {"id":"timer"} or apps app.show {"app":"watch"}, runs that
 * command as if Muse sent it. Synchronous results print as "@apps {...}";
 * asynchronous ones (luapp.install, luapp.run) go nowhere. True if handled. */
bool muse_apps_console(const char *line);

/* How asynchronous results reach Muse (noise_ctrl_send_command_result); it
 * takes ownership of the result. */
typedef void (*muse_apps_reply_fn)(uint64_t generation, const char *request_id, cJSON *result);
void muse_apps_set_reply(muse_apps_reply_fn fn);

/* An HTTP Date header from a reply on Muse's session (any task). Sets the
 * clock when SNTP hasn't: on networks that block NTP, the watch still gets
 * the time, to the second, from the VM's replies. */
void muse_apps_clock_hint(const char *date, size_t len);

/* Sound from the running Lua app, for Muse's voice task while it's idle:
 * fills frames of 16 kHz mono and returns true, or returns false when there's
 * nothing to play (so a call, a note or a reply always has the speaker). */
bool muse_apps_sound_pull(int16_t *out, size_t frames);

#ifdef __cplusplus
}
#endif

/* Hooks in the SDK (patches/0002), for the Lua apps (luapp_dev.c): */
/* Every "@chat" console line of a typed turn (Muse's chat task): muse.ask's answer. */
void muse_apps_chat_line(const char *type, const char *text);
/* The bottom button's short press, screen awake (input task): true if a Lua
 * app on screen took it (ui.on_button) instead of the board sleeping. */
bool muse_apps_aux_press(void);
/* The screen brightness to apply (UI task): a Lua app's app.brightness while
 * it's on screen, else target. */
int muse_apps_brightness(int target);
