#pragma once

/*
 * Lua apps on the "luapps" FAT partition (mounted at /luapps, formatted on
 * first use): <id>.lua (source), <id>.meta (name, colour), <id>.kv (the
 * app's store, JSON), <id>.ico (its launcher icon, see luapp_image.h),
 * <id>.tmp (an install in progress), <id>.fs/ (the app's own files, fs.*),
 * _alarms (every app's alarms). An index of the
 * installed apps is kept in RAM for the launcher and luapp.admin list.
 * File operations may come from any task (a lock takes them one at a time);
 * the index can be read anywhere.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LUAPP_MAX_APPS 64   /* the launcher: pages of 6. Storage (8 MB) is the real limit. */
#define LUAPP_ID_MAX 12
#define LUAPP_NAME_MAX 16
#define LUAPP_CODE_MAX (64 * 1024)
#define LUAPP_KV_MAX (8 * 1024)

typedef struct {
    char id[LUAPP_ID_MAX + 1];
    char name[LUAPP_NAME_MAX + 1];
    uint32_t color;
    uint32_t size;
} luapp_meta_t;

bool luapp_store_mount(void);
bool luapp_store_mounted(void);
/* [a-z0-9_], 1 to 12 characters. */
bool luapp_id_valid(const char *id);
/* Printable ASCII (the screen's fonts have nothing else), 1 to 16 characters. */
bool luapp_name_valid(const char *name);

/* Index (any task). */
int luapp_store_list(luapp_meta_t *out, int cap);
int luapp_store_count(void);
/* The app storage's size and free space, in KB. */
bool luapp_store_space(uint32_t *total_kb, uint32_t *free_kb);
/* The launcher's order: these app ids first, in this order; the others after,
 * as installed. Saved, and applied at once. */
bool luapp_store_set_order(const char *const *ids, int n);
bool luapp_store_find(const char *id, luapp_meta_t *out);
uint32_t luapp_store_version(void);

/* Files (any task, one at a time). Buffers returned are PSRAM, NUL-terminated; free() them. */
bool luapp_store_tmp_write(const char *id, const char *data, size_t len, bool append, size_t *total);
char *luapp_store_tmp_read(const char *id, size_t *len);
void luapp_store_tmp_discard(const char *id);
bool luapp_store_commit(const char *id, const char *name, uint32_t color);
char *luapp_store_code(const char *id, size_t *len);
bool luapp_store_remove(const char *id);
char *luapp_store_kv_read(const char *id, size_t *len);
bool luapp_store_kv_write(const char *id, const char *json, size_t len);
/* The icon file: exactly n bytes, or false. Writing skips an unchanged
 * file (apps set their icon each time they start). */
bool luapp_store_icon_read(const char *id, uint8_t *buf, size_t n);
bool luapp_store_icon_write(const char *id, const uint8_t *buf, size_t n);
void luapp_store_icon_remove(const char *id);

/* An app's own files (fs.*), in <id>.fs/: names of a-z 0-9 _ - . (not
 * starting with a dot), up to 24; 32 files and 256 KB per app, 128 KB per
 * file; 64 KB of the partition always left free. Removing the app removes
 * them. Writing returns NULL, or why it couldn't (static text). */
#define LUAPP_FILE_NAME_MAX 24
#define LUAPP_FILE_MAX (128 * 1024)
#define LUAPP_FILES_MAX 32
#define LUAPP_FILES_QUOTA (256 * 1024)
#define LUAPP_FILES_SPARE (64 * 1024)
typedef struct {
    char name[LUAPP_FILE_NAME_MAX + 1];
    uint32_t size;
} luapp_file_t;
bool luapp_file_name_valid(const char *name);
const char *luapp_store_file_write(const char *id, const char *name, const char *data, size_t len, bool append);
char *luapp_store_file_read(const char *id, const char *name, size_t *len);
bool luapp_store_file_remove(const char *id, const char *name);
/* Fills up to cap entries; returns how many files there are, *total their bytes. */
int luapp_store_file_list(const char *id, luapp_file_t *out, int cap, size_t *total);
/* The host's own files at the top of the partition (no app id). */
char *luapp_store_sys_read(const char *name, size_t max, size_t *len);
bool luapp_store_sys_write(const char *name, const char *data, size_t len);
