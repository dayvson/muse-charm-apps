/*
 * The Apps tile: a launcher of the Lua apps Muse built, and the root the
 * running app's widgets live in. Apps sit 6 to a page (two rows of three
 * 96 px circles: the app's icon, or its initials on its colour); swiping up
 * and down turns pages, with dots and an arrow when there are more. Tapping
 * an app runs it; a long-press anywhere in a running app (or on its error
 * card) comes back to the launcher.
 */

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "luapp.h"
#include "luapp_image.h"
#include "luapp_store.h"

#define GRID LUAPP_MAX_APPS
#define PER_PAGE 6
#define PAGES ((GRID + PER_PAGE - 1) / PER_PAGE)
#define DIM 0x8e8e93
#define TEAL 0x27d3a2
#define DOT_ON 0xa77dff    /* the device's page dots (muse_ui.c) */
#define DOT_OFF 0x3a3358
#define SCREEN 466
#define ICON LUAPP_ICON
#define PITCH_X 128        /* icon centres, across */
#define PITCH_Y 124        /* and down: the circle, its name, a gap */
#define ROW0_Y 163         /* first row's circle centre with two rows */
#define ONE_ROW_Y 225      /* the only row's, a little above the middle */
#define NAME_W 124

static lv_obj_t *s_tile, *s_launcher, *s_scroll, *s_root, *s_empty, *s_dots[PAGES];
static char s_ids[GRID][LUAPP_ID_MAX + 1];
static uint32_t s_shown_version = UINT32_MAX;
static int s_pages, s_page;

/* Icons, by app id: pixels in PSRAM, allocated once per slot and only ever
 * rewritten under the display lock, so what LVGL shows is never freed. */
typedef struct {
    char id[LUAPP_ID_MAX + 1];
    uint8_t *file;   /* LUAPP_ICON_FILE bytes */
    lv_image_dsc_t dsc;
} icon_t;
static icon_t s_icons[GRID];
static bool s_icons_changed;

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, text);
    return l;
}

static icon_t *icon_for(const char *id)
{
    for (int i = 0; i < GRID; i++) {
        if (s_icons[i].id[0] && !strcmp(s_icons[i].id, id)) return &s_icons[i];
    }
    return NULL;
}

bool luapp_ui_set_icon(const char *id, const uint8_t *file)
{
    icon_t *ic = icon_for(id);
    if (!file) {
        if (ic) ic->id[0] = 0;   /* the pixels stay, for the slot's next app */
        s_icons_changed = true;
        return true;
    }
    if (memcmp(file, "LIC", 3)) return false;
    luapp_meta_t m;
    for (int i = 0; i < GRID && !ic; i++) {   /* a free slot, or one whose app is gone */
        if (!s_icons[i].id[0] || !luapp_store_find(s_icons[i].id, &m)) ic = &s_icons[i];
    }
    if (!ic) return false;
    if (!ic->file) ic->file = heap_caps_malloc(LUAPP_ICON_FILE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ic->file) {
        ic->id[0] = 0;
        return false;
    }
    strlcpy(ic->id, id, sizeof(ic->id));
    memcpy(ic->file, file, LUAPP_ICON_FILE);
    memset(&ic->dsc, 0, sizeof(ic->dsc));
    ic->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    ic->dsc.header.cf = LV_COLOR_FORMAT_RGB565A8;
    ic->dsc.header.w = ICON;
    ic->dsc.header.h = ICON;
    ic->dsc.header.stride = ICON * 2;
    ic->dsc.data = ic->file + 4;
    ic->dsc.data_size = LUAPP_ICON_PX;
    s_icons_changed = true;
    return true;
}

static void on_icon(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i >= 0 && i < GRID && s_ids[i][0]) luapp_vm_run(s_ids[i]);
}

static void on_root_tap(lv_event_t *e)
{
    if (lv_event_get_target(e) != s_root) return;   /* a button's own tap bubbled up */
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    luapp_vm_tap(-1, luapp_vm_gen(), p.x - 233, p.y - 233);
}

static void on_root_long(lv_event_t *e)
{
    (void)e;
    luapp_vm_tap(-2, luapp_vm_gen(), 0, 0);
}

static void show_dots(void)
{
    for (int i = 0; i < PAGES; i++) {
        lv_obj_set_flag(s_dots[i], LV_OBJ_FLAG_HIDDEN, s_pages < 2 || i >= s_pages);
        lv_obj_set_style_bg_color(s_dots[i], lv_color_hex(i == s_page ? DOT_ON : DOT_OFF), 0);
        lv_obj_align(s_dots[i], LV_ALIGN_RIGHT_MID, -14, 16 * i - 8 * (s_pages - 1));
    }
}

static void on_scroll(lv_event_t *e)
{
    (void)e;
    int page = (lv_obj_get_scroll_y(s_scroll) + SCREEN / 2) / SCREEN;
    if (page < 0) page = 0;
    if (page >= s_pages) page = s_pages - 1;
    if (page != s_page) {
        s_page = page;
        show_dots();
    }
}

static void bob(void *obj, int32_t v) { lv_obj_set_style_translate_y(obj, v, 0); }

/* "More below": a chevron that bobs now and then. */
static void more_arrow(lv_obj_t *page)
{
    lv_obj_t *a = label(page, &lv_font_montserrat_20, TEAL, LV_SYMBOL_DOWN);
    lv_obj_align(a, LV_ALIGN_TOP_MID, 0, 376);
    lv_anim_t an;
    lv_anim_init(&an);
    lv_anim_set_var(&an, a);
    lv_anim_set_exec_cb(&an, bob);
    lv_anim_set_values(&an, 0, 7);
    lv_anim_set_duration(&an, 450);
    lv_anim_set_reverse_duration(&an, 450);
    lv_anim_set_repeat_count(&an, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_repeat_delay(&an, 1600);
    lv_anim_set_path_cb(&an, lv_anim_path_ease_in_out);
    lv_anim_start(&an);
}

static uint32_t shade(uint32_t c, int pct)
{
    return (((c >> 16 & 255) * pct / 100) << 16) | (((c >> 8 & 255) * pct / 100) << 8) | ((c & 255) * pct / 100);
}

static void make_cell(lv_obj_t *page, int i, const luapp_meta_t *app, int cx, int cy)
{
    lv_obj_t *cell = lv_obj_create(page);
    lv_obj_remove_style_all(cell);
    lv_obj_set_size(cell, PITCH_X, ICON + 24);
    lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_opa(cell, LV_OPA_60, LV_STATE_PRESSED);
    lv_obj_set_pos(cell, cx - PITCH_X / 2, cy - ICON / 2);
    lv_obj_add_event_cb(cell, on_icon, LV_EVENT_CLICKED, (void *)(intptr_t)i);

    /* The disc: the app's colour, lit from above, with a faint rim drawn
     * over whatever is on it. */
    uint32_t c = app->color;
    icon_t *ic = icon_for(app->id);
    lv_obj_t *dot = lv_obj_create(cell);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, ICON, ICON);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(c), 0);
    lv_obj_set_style_bg_grad_color(dot, lv_color_hex(shade(c, 62)), 0);
    lv_obj_set_style_bg_grad_dir(dot, LV_GRAD_DIR_VER, 0);
    bool fills = ic && (ic->file[3] & LUAPP_ICON_FILLS);
    lv_obj_set_style_bg_opa(dot, fills ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(dot, lv_color_white(), 0);
    lv_obj_set_style_border_opa(dot, 46, 0);
    lv_obj_set_style_border_width(dot, 2, 0);
    lv_obj_set_style_border_post(dot, true, 0);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(dot, LV_ALIGN_TOP_MID, 0, 0);
    if (ic) {
        lv_obj_t *im = lv_image_create(dot);
        lv_image_set_src(im, &ic->dsc);
        lv_obj_center(im);
    } else {
        /* Initials: the first letters of the first two words. */
        char ini[3] = {0};
        const char *nm = app->name;
        ini[0] = (char)toupper((unsigned char)nm[0]);
        const char *sp = strchr(nm, ' ');
        if (sp && sp[1]) ini[1] = (char)toupper((unsigned char)sp[1]);
        else if (nm[1]) ini[1] = (char)tolower((unsigned char)nm[1]);
        int lum = (int)(((c >> 16) & 255) * 299 + ((c >> 8) & 255) * 587 + (c & 255) * 114) / 1000;
        lv_obj_t *il = label(dot, &lv_font_montserrat_28, lum > 150 ? 0x000000 : 0xffffff, ini);
        lv_obj_center(il);
    }
    lv_obj_t *nl = label(cell, &lv_font_montserrat_16, 0xffffff, app->name);
    lv_obj_set_size(nl, NAME_W, lv_font_get_line_height(&lv_font_montserrat_16));   /* one line */
    lv_label_set_long_mode(nl, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(nl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(nl, LV_ALIGN_TOP_MID, 0, ICON + 4);
}

static void rebuild(void)
{
    luapp_meta_t apps[LUAPP_MAX_APPS];
    int n = luapp_store_list(apps, LUAPP_MAX_APPS);
    if (n > GRID) n = GRID;
    lv_obj_clean(s_scroll);   /* pages, cells, arrows (and their animations) */
    for (int i = 0; i < GRID; i++) s_ids[i][0] = 0;
    lv_obj_set_flag(s_empty, LV_OBJ_FLAG_HIDDEN, n > 0);
    s_pages = n ? (n + PER_PAGE - 1) / PER_PAGE : 1;
    for (int p = 0; p < s_pages; p++) {
        lv_obj_t *page = lv_obj_create(s_scroll);
        lv_obj_remove_style_all(page);
        lv_obj_set_size(page, SCREEN, SCREEN);
        lv_obj_set_pos(page, 0, p * SCREEN);
        lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        int first = p * PER_PAGE, k = n - first < PER_PAGE ? n - first : PER_PAGE;   /* apps on this page */
        int rows = k > 3 ? 2 : 1, y0 = rows == 2 ? ROW0_Y : ONE_ROW_Y;
        if (p == 0) {
            lv_obj_t *t = label(page, &lv_font_montserrat_16, TEAL, "APPS BY MUSE");
            lv_obj_set_style_text_letter_space(t, 2, 0);
            lv_obj_align(t, LV_ALIGN_TOP_MID, 0, n ? y0 - ICON / 2 - 34 : 36);
        } else {
            lv_obj_t *up = label(page, &lv_font_montserrat_16, DIM, LV_SYMBOL_UP);
            lv_obj_align(up, LV_ALIGN_TOP_MID, 0, y0 - ICON / 2 - 36);
        }
        for (int j = 0; j < k; j++) {
            int row = j / 3, col = j % 3, in_row = row == 0 ? (k < 3 ? k : 3) : k - 3;
            int cx = SCREEN / 2 + PITCH_X * col - PITCH_X / 2 * (in_row - 1);   /* centred */
            make_cell(page, first + j, &apps[first + j], cx, y0 + row * PITCH_Y);
            strlcpy(s_ids[first + j], apps[first + j].id, sizeof(s_ids[0]));
        }
        if (p < s_pages - 1) more_arrow(page);
    }
    if (s_page >= s_pages) s_page = s_pages - 1;
    lv_obj_update_layout(s_scroll);
    lv_obj_scroll_to_y(s_scroll, s_page * SCREEN, LV_ANIM_OFF);
    show_dots();
}

static void tick(lv_timer_t *t)
{
    (void)t;
    uint32_t v = luapp_store_version();
    if (v != s_shown_version || s_icons_changed) {
        s_shown_version = v;
        s_icons_changed = false;
        rebuild();
    }
}

void luapp_ui_build(lv_obj_t *tile, int w, int h)
{
    s_tile = tile;
    lv_obj_set_style_bg_color(tile, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(tile, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_all(tile, 0, 0);

    s_launcher = lv_obj_create(tile);
    lv_obj_remove_style_all(s_launcher);
    lv_obj_set_size(s_launcher, w, h);
    lv_obj_remove_flag(s_launcher, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    /* Pages of icons, one per swipe up or down; sideways swipes still change
     * screens. */
    s_scroll = lv_obj_create(s_launcher);
    lv_obj_remove_style_all(s_scroll);
    lv_obj_set_size(s_scroll, w, h);
    lv_obj_set_scroll_dir(s_scroll, LV_DIR_VER);
    lv_obj_set_scroll_snap_y(s_scroll, LV_SCROLL_SNAP_START);
    lv_obj_add_flag(s_scroll, LV_OBJ_FLAG_SCROLL_ONE);
    lv_obj_set_scrollbar_mode(s_scroll, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(s_scroll, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_scroll, on_scroll, LV_EVENT_SCROLL, NULL);
    for (int i = 0; i < PAGES; i++) {
        lv_obj_t *d = lv_obj_create(s_launcher);
        lv_obj_remove_style_all(d);
        lv_obj_set_size(d, 8, 8);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_remove_flag(d, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(d, LV_OBJ_FLAG_HIDDEN);
        s_dots[i] = d;
    }

    s_empty = lv_obj_create(s_launcher);
    lv_obj_remove_style_all(s_empty);
    lv_obj_set_size(s_empty, 340, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_empty, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_empty, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_empty, 10, 0);
    lv_obj_remove_flag(s_empty, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(s_empty);
    label(s_empty, &lv_font_montserrat_20, 0xffffff, "No apps yet");
    lv_obj_t *hint = label(s_empty, &lv_font_montserrat_14, DIM,
                           "Ask Muse to build one, like \"build me a pomodoro timer for my gadget\".");
    lv_obj_set_width(hint, 300);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);

    s_root = lv_obj_create(tile);
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, w, h);
    lv_obj_set_style_bg_color(s_root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_root, on_root_tap, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_add_event_cb(s_root, on_root_long, LV_EVENT_LONG_PRESSED, NULL);

    lv_timer_create(tick, 500, NULL);
    luapp_vm_start();
}

lv_obj_t *luapp_ui_app_root(void) { return s_root; }

void luapp_ui_show_app(void)
{
    lv_obj_set_style_bg_color(s_root, lv_color_black(), 0);
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_launcher, LV_OBJ_FLAG_HIDDEN);
}

void luapp_ui_show_launcher(void)
{
    lv_obj_clean(s_root);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_launcher, LV_OBJ_FLAG_HIDDEN);
}

void luapp_ui_show_error(const char *app_name, const char *error)
{
    luapp_ui_show_app();
    lv_obj_t *t = label(s_root, &lv_font_montserrat_20, 0xff9f2e, "");
    lv_label_set_text_fmt(t, "%s stopped", app_name);
    lv_obj_align(t, LV_ALIGN_CENTER, 0, -90);
    char msg[260];
    strlcpy(msg, error, sizeof(msg));   /* the first lines; luapp.admin logs has it all */
    lv_obj_t *m = label(s_root, &lv_font_montserrat_14, 0xd0d0d0, msg);
    lv_obj_set_width(m, 330);
    lv_label_set_long_mode(m, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(m, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(m, LV_ALIGN_CENTER, 0, 0);
    lv_obj_t *hint = label(s_root, &lv_font_montserrat_14, DIM, "Long-press to go back");
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 130);
}
