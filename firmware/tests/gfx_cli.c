/*
 * Host harness for luapp_image.c and luapp_synth.c (driven by test_gfx.py):
 *   gfx_cli decode IN OUT [max_dim]   -> OUT: "w h alpha\n" + pixels, or "error: ..."
 *   gfx_cli sprite OUT                -> a fixed sprite, same format
 *   gfx_cli icon IN OUT               -> IN decoded, then OUT: the launcher icon file
 *   gfx_cli melody "C4:8 ..." bpm     -> one "f0 ms" line per note, or "error: ..."
 *   gfx_cli synth                     -> self-checks of the mixer; exit status
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "luapp_image.h"
#include "luapp_synth.h"

static void *alloc(size_t n, void *ud)
{
    size_t cap = *(size_t *)ud;
    return n > cap ? NULL : malloc(n);
}

static int write_px(const char *path, bool ok, const luapp_pixels_t *px, const char *err)
{
    FILE *f = fopen(path, "wb");
    if (!f) return 2;
    if (!ok) {
        fprintf(f, "error: %s\n", err);
    } else {
        fprintf(f, "%d %d %d\n", px->w, px->h, px->alpha);
        fwrite(px->px, 1, px->bytes, f);
    }
    fclose(f);
    return 0;
}

static int decode(const char *in, const char *out, int max_dim, size_t cap)
{
    FILE *f = fopen(in, "rb");
    if (!f) return 2;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc((size_t)n);
    if (fread(d, 1, (size_t)n, f) != (size_t)n) return 2;
    fclose(f);
    luapp_pixels_t px;
    const char *err = NULL;
    bool ok = luapp_img_decode(d, (size_t)n, max_dim, alloc, &cap, &px, &err);
    int rc = write_px(out, ok, &px, err);
    free(px.px);
    free(d);
    return rc;
}

static int icon(const char *in, const char *out)
{
    FILE *f = fopen(in, "rb");
    if (!f) return 2;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc((size_t)n);
    if (fread(d, 1, (size_t)n, f) != (size_t)n) return 2;
    fclose(f);
    size_t cap = 1 << 22;
    luapp_pixels_t px;
    const char *err = NULL;
    if (!luapp_img_decode(d, (size_t)n, 466, alloc, &cap, &px, &err)) {
        fprintf(stderr, "error: %s\n", err);
        return 1;
    }
    static uint8_t file[LUAPP_ICON_FILE];
    luapp_icon_make(&px, file);
    f = fopen(out, "wb");
    fwrite(file, 1, sizeof(file), f);
    fclose(f);
    free(px.px);
    free(d);
    return 0;
}

static int sprite(const char *out)
{
    const char *rows[] = {"..#..", ".###.", "#####", "#.R.#"};
    size_t lens[] = {5, 5, 5, 5};
    uint32_t pal[256] = {0};
    bool has[256] = {0};
    pal['#'] = 0x5ac8fa;
    has['#'] = true;
    pal['R'] = 0xff0000;
    has['R'] = true;
    size_t cap = 1 << 20;
    luapp_pixels_t px;
    const char *err = NULL;
    bool ok = luapp_img_sprite(rows, lens, 4, pal, has, 3, 466, alloc, &cap, &px, &err);
    int rc = write_px(out, ok, &px, err);
    free(px.px);
    return rc;
}

static int melody(const char *s, int bpm)
{
    synth_note_t notes[SYNTH_NOTES];
    char err[120];
    int n = synth_melody(s, bpm, WAVE_SQUARE, 40, notes, SYNTH_NOTES, err, sizeof(err));
    if (n < 0) {
        printf("error: %s\n", err);
        return 0;
    }
    for (int i = 0; i < n; i++) printf("%d %d\n", notes[i].f0, notes[i].ms);
    return 0;
}

#define CHECK(c) do { if (!(c)) { printf("synth check failed: %s (line %d)\n", #c, __LINE__); return 1; } } while (0)

static int synth(void)
{
    int16_t buf[320];
    synth_init();
    CHECK(!synth_render(buf, 320));           /* silent: nothing to play */
    CHECK(!synth_busy(-1));
    synth_note_t n[8];
    CHECK(synth_effect("nope", 50, n, 8) < 0);
    for (const char *e = synth_effect_names; *e;) {   /* every listed effect exists */
        char name[16];
        int k = 0;
        while (*e && *e != ',') name[k++] = *e++;
        name[k] = 0;
        while (*e == ',' || *e == ' ') e++;
        CHECK(synth_effect(name, 50, n, 8) > 0);
    }
    int c = synth_effect("coin", 60, n, 8);
    synth_play(3, n, c, false);
    CHECK(synth_busy(3) && !synth_busy(0));
    int peak = 0, frames = 0;
    while (synth_render(buf, 320)) {
        for (int i = 0; i < 320; i++) peak = abs(buf[i]) > peak ? abs(buf[i]) : peak;
        frames += 320;
        CHECK(frames < 16000);                   /* coin is 280 ms */
    }
    CHECK(peak > 2000 && peak < 32767);
    CHECK(frames >= 4160 && frames <= 4800);  /* 280 ms = 4480 frames, in 320-frame chunks */
    CHECK(!synth_busy(-1));
    /* A loop keeps going until stopped; every wave renders. */
    for (int w = 0; w < WAVE_COUNT; w++) {
        synth_note_t t = {.f0 = 440, .f1 = 880, .ms = 50, .wave = (uint8_t)w, .vol = 100, .fade = false};
        synth_play(0, &t, 1, true);
        for (int i = 0; i < 20; i++) CHECK(synth_render(buf, 320));
        CHECK(synth_busy(0));
        synth_stop(0);
        CHECK(!synth_busy(-1) && !synth_render(buf, 320));
    }
    /* All four channels at full volume clip instead of wrapping. */
    synth_note_t loud = {.f0 = 200, .f1 = 200, .ms = 500, .wave = WAVE_SQUARE, .vol = 100};
    for (int ch = 0; ch < SYNTH_CH; ch++) synth_play(ch, &loud, 1, false);
    synth_render(buf, 320);
    synth_render(buf, 320);
    for (int i = 0; i < 320; i++) CHECK(buf[i] == 0 || abs(buf[i]) > 1000);
    synth_stop(-1);
    CHECK(synth_wave("sine") == WAVE_SINE && synth_wave("x") < 0);
    printf("synth: mixer, effects, loops and clipping OK\n");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 4 && !strcmp(argv[1], "decode")) {
        return decode(argv[2], argv[3], argc > 4 ? atoi(argv[4]) : 466, argc > 5 ? (size_t)atol(argv[5]) : 1 << 22);
    }
    if (argc == 3 && !strcmp(argv[1], "sprite")) return sprite(argv[2]);
    if (argc == 4 && !strcmp(argv[1], "icon")) return icon(argv[2], argv[3]);
    if (argc == 4 && !strcmp(argv[1], "melody")) return melody(argv[2], atoi(argv[3]));
    if (argc == 2 && !strcmp(argv[1], "synth")) return synth();
    fprintf(stderr, "usage: see the top of gfx_cli.c\n");
    return 2;
}
