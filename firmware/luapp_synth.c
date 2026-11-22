#include "luapp_synth.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
static SemaphoreHandle_t s_lock;
static void lock(void) { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { if (s_lock) xSemaphoreGive(s_lock); }
#define PSRAM_BSS EXT_RAM_BSS_ATTR
#else
static void lock(void) {}
static void unlock(void) {}
#define PSRAM_BSS
#endif

#define ATTACK 48      /* 3 ms */
#define RELEASE 128    /* 8 ms */

typedef struct {
    synth_note_t notes[SYNTH_NOTES];
    int n, idx;
    bool loop, active;
    uint32_t pos, len;
    uint32_t phase, lfsr;
    float noise;
} chan_t;

PSRAM_BSS static chan_t s_ch[SYNTH_CH];
static volatile bool s_any;   /* a cheap "anything playing?" for the voice task */
PSRAM_BSS static int16_t s_sine[256];

void synth_init(void)
{
#ifdef ESP_PLATFORM
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
#endif
    for (int i = 0; i < 256; i++) s_sine[i] = (int16_t)lrintf(32767.0f * sinf(2.0f * (float)M_PI * i / 256.0f));
    memset(s_ch, 0, sizeof(s_ch));
    s_any = false;
}

static void start_note(chan_t *c)
{
    c->pos = 0;
    c->len = (uint32_t)c->notes[c->idx].ms * (SYNTH_RATE / 1000);
    if (c->len == 0) c->len = 1;
}

static void refresh_any(void)
{
    bool any = false;
    for (int i = 0; i < SYNTH_CH; i++) any |= s_ch[i].active;
    s_any = any;
}

void synth_play(int ch, const synth_note_t *notes, int n, bool loop)
{
    if (ch < 0 || ch >= SYNTH_CH || n <= 0) return;
    if (n > SYNTH_NOTES) n = SYNTH_NOTES;
    lock();
    chan_t *c = &s_ch[ch];
    memcpy(c->notes, notes, sizeof(notes[0]) * (size_t)n);
    c->n = n;
    c->idx = 0;
    c->loop = loop;
    c->active = true;
    if (!c->lfsr) c->lfsr = 0x7a3d1u + (uint32_t)ch;
    start_note(c);
    s_any = true;
    unlock();
}

void synth_stop(int ch)
{
    lock();
    for (int i = 0; i < SYNTH_CH; i++) {
        if (ch < 0 || ch == i) s_ch[i].active = false;
    }
    refresh_any();
    unlock();
}

bool synth_busy(int ch)
{
    lock();
    bool b = false;
    for (int i = 0; i < SYNTH_CH; i++) {
        if ((ch < 0 || ch == i) && s_ch[i].active) b = true;
    }
    unlock();
    return b;
}

/* Per-wave loudness, so a sine at 50 sounds about as loud as a square at 50. */
static const float GAIN[WAVE_COUNT] = {0.55f, 0.5f, 1.0f, 0.6f, 1.0f, 0.45f};

bool synth_render(int16_t *out, size_t frames)
{
    if (!s_any) return false;
    lock();
    bool played = false;
    for (size_t i = 0; i < frames; i++) {
        float mix = 0;
        for (int k = 0; k < SYNTH_CH; k++) {
            chan_t *c = &s_ch[k];
            if (!c->active) continue;
            played = true;
            const synth_note_t *nt = &c->notes[c->idx];
            if (nt->f0) {
                float t = (float)c->pos / (float)c->len;
                float f = nt->f0 + ((float)nt->f1 - (float)nt->f0) * t;
                uint32_t inc = (uint32_t)(f * (4294967296.0f / SYNTH_RATE));
                uint32_t prev = c->phase;
                c->phase += inc;
                float v;
                switch (nt->wave) {
                case WAVE_PULSE: v = c->phase < 0x40000000u ? 1.0f : -1.0f; break;
                case WAVE_TRIANGLE: {
                    uint32_t p = c->phase;
                    v = (p < 0x80000000u ? (float)p : (float)(0xffffffffu - p)) / 1073741824.0f - 1.0f;
                    break;
                }
                case WAVE_SAW: v = (float)c->phase / 2147483648.0f - 1.0f; break;
                case WAVE_SINE: v = s_sine[c->phase >> 24] / 32767.0f; break;
                case WAVE_NOISE:
                    if (c->phase < prev) {   /* wrapped: a new random level, NES style */
                        c->lfsr ^= c->lfsr << 13;
                        c->lfsr ^= c->lfsr >> 17;
                        c->lfsr ^= c->lfsr << 5;
                        c->noise = (c->lfsr & 1) ? 1.0f : -1.0f;
                    }
                    v = c->noise;
                    break;
                default: v = c->phase < 0x80000000u ? 1.0f : -1.0f; break;
                }
                float env = 1.0f;
                if (c->pos < ATTACK) env = (float)c->pos / ATTACK;
                uint32_t left = c->len - c->pos;
                if (left < RELEASE) env *= (float)left / RELEASE;
                if (nt->fade) env *= 1.0f - (float)c->pos / (float)c->len;
                mix += v * env * GAIN[nt->wave < WAVE_COUNT ? nt->wave : 0] * nt->vol * 150.0f;
            }
            if (++c->pos >= c->len) {
                if (++c->idx >= c->n) {
                    if (c->loop) {
                        c->idx = 0;
                    } else {
                        c->active = false;
                        continue;
                    }
                }
                start_note(c);
            }
        }
        out[i] = (int16_t)(mix > 32767 ? 32767 : mix < -32768 ? -32768 : mix);
    }
    refresh_any();
    unlock();
    return played;
}

int synth_wave(const char *name)
{
    static const char *const N[WAVE_COUNT] = {"square", "pulse", "triangle", "saw", "sine", "noise"};
    for (int i = 0; i < WAVE_COUNT; i++) {
        if (!strcmp(name, N[i])) return i;
    }
    return -1;
}

/* ---- melodies ---- */

int synth_melody(const char *s, int bpm, int wave, int vol, synth_note_t *out, int cap, char *err_buf,
                 size_t err_len)
{
    static const int SEMI[7] = {9, 11, 0, 2, 4, 5, 7};   /* A B C D E F G */
    int octave = 4, length = 4, n = 0, idx = 0;
    if (bpm < 20) bpm = 20;
    if (bpm > 600) bpm = 600;
    while (*s) {
        while (*s == ' ' || *s == ',' || *s == '\n' || *s == '\t' || *s == '|') s++;
        if (!*s) break;
        const char *tok = s;
        idx++;
        char c = (char)toupper((unsigned char)*s++);
        int semi = -1;
        bool rest = false;
        if (c >= 'A' && c <= 'G') {
            semi = SEMI[c - 'A'];
            if (*s == '#') {
                semi++;
                s++;
            } else if (*s == 'b') {
                semi--;
                s++;
            }
        } else if (c == 'R' || c == '-') {
            rest = true;
        } else {
            goto bad;
        }
        if (isdigit((unsigned char)*s)) octave = *s++ - '0';
        if (*s == ':') {
            s++;
            if (!isdigit((unsigned char)*s)) goto bad;
            int l = 0;
            while (isdigit((unsigned char)*s)) l = l * 10 + (*s++ - '0');
            if (l != 1 && l != 2 && l != 4 && l != 8 && l != 16 && l != 32) goto bad;
            length = l;
        }
        bool dotted = false;
        if (*s == '.') {
            dotted = true;
            s++;
        }
        if (*s && *s != ' ' && *s != ',' && *s != '\n' && *s != '\t' && *s != '|') goto bad;
        if (n >= cap) {
            snprintf(err_buf, err_len, "melody: more than %d notes", cap);
            return -1;
        }
        int ms = 240000 / (bpm * length);
        if (dotted) ms = ms * 3 / 2;
        synth_note_t *nt = &out[n++];
        memset(nt, 0, sizeof(*nt));
        nt->ms = (uint16_t)(ms < 10 ? 10 : ms);
        nt->wave = (uint8_t)wave;
        nt->vol = (uint8_t)vol;
        if (!rest) {
            int midi = 12 * (octave + 1) + semi;
            float f = 440.0f * powf(2.0f, (midi - 69) / 12.0f);
            if (f > 7900) f = 7900;
            nt->f0 = nt->f1 = (uint16_t)lrintf(f);
        }
        continue;
    bad:;
        int k = 0;
        while (tok[k] && tok[k] != ' ' && k < 12) k++;
        snprintf(err_buf, err_len, "melody: can't read \"%.*s\" (note %d); use notes like C4:8, D#5:4. or R:8", k,
                 tok, idx);
        return -1;
    }
    if (!n) {
        snprintf(err_buf, err_len, "melody: no notes");
        return -1;
    }
    return n;
}

/* ---- effects ---- */

typedef struct {
    uint16_t f0, f1, ms;
    uint8_t wave;
    bool fade;
} fx_t;

#define SQ WAVE_SQUARE
#define PU WAVE_PULSE
#define TR WAVE_TRIANGLE
#define SW WAVE_SAW
#define NZ WAVE_NOISE

static const fx_t FX_CLICK[] = {{1800, 1800, 14, SQ, true}};
static const fx_t FX_SELECT[] = {{880, 1320, 50, PU, false}};
static const fx_t FX_COIN[] = {{988, 988, 60, SQ, false}, {1319, 1319, 220, SQ, true}};
static const fx_t FX_JUMP[] = {{260, 760, 160, SQ, true}};
static const fx_t FX_LASER[] = {{1600, 220, 180, SW, true}};
static const fx_t FX_HIT[] = {{1400, 600, 120, NZ, true}};
static const fx_t FX_EXPLOSION[] = {{900, 90, 600, NZ, true}};
static const fx_t FX_POWERUP[] = {{523, 523, 60, SQ, false}, {659, 659, 60, SQ, false}, {784, 784, 60, SQ, false},
                                  {1047, 1047, 60, SQ, false}, {1319, 1319, 160, SQ, true}};
static const fx_t FX_WIN[] = {{523, 523, 110, PU, false}, {659, 659, 110, PU, false}, {784, 784, 110, PU, false},
                              {1047, 1047, 420, PU, true}};
static const fx_t FX_LOSE[] = {{392, 392, 180, TR, false}, {370, 370, 180, TR, false}, {349, 349, 180, TR, false},
                               {330, 200, 600, TR, true}};
static const fx_t FX_ERROR[] = {{220, 220, 110, SQ, false}, {0, 0, 50, SQ, false}, {220, 220, 170, SQ, true}};
static const fx_t FX_TICK[] = {{2400, 2400, 8, TR, false}};

static const struct {
    const char *name;
    const fx_t *fx;
    int n;
} FX[] = {
#define E(nm, a) {nm, a, (int)(sizeof(a) / sizeof(a[0]))}
    E("click", FX_CLICK), E("select", FX_SELECT), E("coin", FX_COIN), E("jump", FX_JUMP),
    E("laser", FX_LASER), E("hit", FX_HIT), E("explosion", FX_EXPLOSION), E("powerup", FX_POWERUP),
    E("win", FX_WIN), E("lose", FX_LOSE), E("error", FX_ERROR), E("tick", FX_TICK),
#undef E
};

const char *const synth_effect_names =
    "click, select, coin, jump, laser, hit, explosion, powerup, win, lose, error, tick";

int synth_effect(const char *name, int vol, synth_note_t *out, int cap)
{
    for (size_t i = 0; i < sizeof(FX) / sizeof(FX[0]); i++) {
        if (strcmp(name, FX[i].name)) continue;
        int n = FX[i].n < cap ? FX[i].n : cap;
        for (int k = 0; k < n; k++) {
            const fx_t *f = &FX[i].fx[k];
            out[k] = (synth_note_t){.f0 = f->f0, .f1 = f->f1, .ms = f->ms, .wave = f->wave, .vol = (uint8_t)vol,
                                    .fade = f->fade};
        }
        return n;
    }
    return -1;
}
