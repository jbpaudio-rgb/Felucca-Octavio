/* SPDX-License-Identifier: MIT */
/* KIT engine test: every type x random parameters, a voice must stay bounded, end by itself, never touch
 * memory it should not (build with -fsanitize=address,undefined), and report its cost.
 *   cc -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -std=c99 -Ibuild -o build/kit_test tests/kit_test.c */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define NVOICE 8
#define NPART 4
#include "fe_tables.h"
#include "../src/felucca_engine.h"
enum { P_ATK, P_REL, P_FTYPE, P_E0, P_E1, P_E2, P_E3, P_E4, P_E5, P_E6, P_E7, P_COUNT };
typedef struct track { int16_t p[P_COUNT]; voice_t v[NVOICE]; } track_t;
static track_t trk[NPART];
#include "../src/dsp.c"
#include "../src/eng_kit.c"

static uint32_t rs = 12345;
static uint32_t rnd(void) { rs = rs * 1664525u + 1013904223u; return rs >> 8; }

/* returns blocks the hit lasted (cap 20000), writes peak of one voice and the max step */
static int hit(track_t *t, int32_t *peak, int32_t *zc)
{
    voice_t *v = &t->v[0];
    int32_t out[CTL], prev = 0;
    int b, i;
    memset(v, 0, sizeof *v);
    v->note = (uint8_t)(rnd() % 128); v->vel = 100; v->gate = 1; v->active = 1; v->stage = 1; v->age = rnd();
    v->pitch16 = v->pitch_cur = v->note * 16;
    ENG_KIT.note_on(t, v);
    *peak = 0; *zc = 0;
    for (b = 0; b < 20000 && v->active; b++) {
        vmod_t m; int32_t a = ENG_KIT.amp(t, v, 1 << 15);
        if (!v->active) break;
        memset(&m, 0, sizeof m); m.amp0 = m.amp1 = a; m.pitch16 = v->pitch16;
        memset(out, 0, sizeof out);
        ENG_KIT.render(t, v, out, CTL, &m);
        for (i = 0; i < CTL; i++) {
            int32_t x = abs(out[i]);
            if (x > *peak) *peak = x;
            if ((out[i] ^ prev) < 0) (*zc)++;
            prev = out[i];
        }
    }
    return b;
}

int main(void)
{
    int n, k, worst_len = 0, worst_peak = 0, bad = 0;
    track_t *t = &trk[0];
    long zc_ty[K_NTYPE] = {0};
    for (n = 0; n < 40000; n++) {
        int32_t peak, zc, len;
        for (k = 0; k < 8; k++) t->p[P_E0 + k] = (int16_t)(rnd() % 128);
        t->p[P_E1] = (int16_t)((int32_t)(rnd() % 128) - 64);
        t->p[P_E0] = n % K_NTYPE;
        if (n < K_NTYPE) { for (k = 1; k < 8; k++) t->p[P_E0 + k] = ENG_KIT.edit[k].def; t->p[P_E7] = 0; }
        len = hit(t, &peak, &zc);
        if (len >= 20000) { bad++; printf("never ends: ty=%d dc=%d\n", t->p[P_E0], t->p[P_E2]); }
        if (len > worst_len) worst_len = len;
        if (peak > worst_peak) worst_peak = peak;
        if (peak > 32767) { bad++; if (bad < 5) printf("peak %d > 32767 ty=%d\n", peak, t->p[P_E0]); }
        if (n < K_NTYPE) zc_ty[n] = zc;
    }
    for (k = 0; k < K_NTYPE; k++) printf("type %s default: zero-crossings %ld\n", N_KIT_TYPE[k], zc_ty[k]);
    /* cost: 8 voices of the dearest type for 1 s */
    {
        voice_t vs[8]; int32_t out[CTL]; clock_t c0; int b, j; vmod_t m;
        t->p[P_E0] = K_OH; for (k = 1; k < 8; k++) t->p[P_E0 + k] = ENG_KIT.edit[k].def; t->p[P_E5] = 100; t->p[P_E2] = 127;
        memset(vs, 0, sizeof vs);
        for (j = 0; j < 8; j++) { vs[j].active = 1; vs[j].note = 60; vs[j].age = j + 1; ENG_KIT.note_on(t, &vs[j]); }
        memset(&m, 0, sizeof m); m.amp0 = m.amp1 = 20000;
        c0 = clock();
        for (b = 0; b < FS / CTL; b++) { memset(out, 0, sizeof out); for (j = 0; j < 8; j++) ENG_KIT.render(t, &vs[j], out, CTL, &m); }
        printf("8 voices OH, 1 s of audio: %.2f ms host time\n", (double)(clock() - c0) * 1000.0 / CLOCKS_PER_SEC);
    }
    printf("40000 hits: longest %d blocks (%.1f s), peak %d, problems %d\n", worst_len, worst_len * 32.0 / FS, worst_peak, bad);
    return bad != 0;
}
