#pragma once

/*
 * A small game synth for Lua apps: 4 channels of square, pulse, triangle,
 * saw, sine or noise voices with pitch slides and fades, mixed to 16 kHz
 * mono. Each channel plays a list of notes (a tone, an effect or a melody);
 * a new list replaces what the channel was playing.
 *
 * synth_render runs on Muse's voice task, everything else on the Lua task:
 * the channels are guarded by a lock (a FreeRTOS mutex on the board).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SYNTH_RATE 16000
#define SYNTH_CH 4
#define SYNTH_NOTES 128

enum { WAVE_SQUARE, WAVE_PULSE, WAVE_TRIANGLE, WAVE_SAW, WAVE_SINE, WAVE_NOISE, WAVE_COUNT };

typedef struct {
    uint16_t f0, f1;    /* Hz; f0 == 0 is a rest; f1 != f0 slides */
    uint16_t ms;
    uint8_t wave, vol;  /* vol 0..100 */
    bool fade;          /* fade out across the note */
} synth_note_t;

void synth_init(void);
/* Replaces channel ch's sound (0..SYNTH_CH-1). */
void synth_play(int ch, const synth_note_t *notes, int n, bool loop);
void synth_stop(int ch);          /* -1: all */
bool synth_busy(int ch);          /* -1: any */
/* Mixes the next frames; false (out untouched) when nothing is playing. */
bool synth_render(int16_t *out, size_t frames);

int synth_wave(const char *name);   /* -1 if unknown */
/* "C4:8 E4:8 G4:4. R:8 C5:2" (see luapp_api_sound.md). Returns the note
 * count, or -1 with *err set (static text, or err_buf). */
int synth_melody(const char *s, int bpm, int wave, int vol, synth_note_t *out, int cap, char *err_buf,
                 size_t err_len);
/* Preset sound effects; -1 if the name is unknown. */
int synth_effect(const char *name, int vol, synth_note_t *out, int cap);
extern const char *const synth_effect_names;
