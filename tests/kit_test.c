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
static int32_t lane_lv = 127;
#define KIT_LANE_LEVEL(t, lane) lane_lv
static int32_t snd_dc = 50, snd_tune = 0;
#define KIT_LANE_SND(t, lane, k, def) ((k) == 1 ? snd_dc : (k) == 0 ? snd_tune : (def))
#include "../src/eng_kit.c"

static uint32_t rs = 12345;
static uint32_t rnd(void) { rs = rs * 1664525u + 1013904223u; return rs >> 8; }

/* returns blocks the hit lasted (cap 20000), writes peak of one voice and the max step */
static int fixed_note = -1;
static int hit(track_t *t, int32_t *peak, int32_t *zc)
{
    voice_t *v = &t->v[0];
    int32_t out[CTL], prev = 0;
    int b, i;
    memset(v, 0, sizeof *v);
    v->note = fixed_note >= 0 ? (uint8_t)fixed_note : (uint8_t)(rnd() % 128); v->vel = 100; v->gate = 1; v->active = 1; v->stage = 1; v->age = rnd();
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
    long zc_ty[K_NTYPE + 1] = {0};
    for (n = 0; n < 40000; n++) {
        int32_t peak, zc, len;
        for (k = 0; k < 8; k++) t->p[P_E0 + k] = (int16_t)(rnd() % 128);
        t->p[P_E1] = (int16_t)((int32_t)(rnd() % 128) - 64);
        t->p[P_E0] = n % (K_KITMODE + 1);
        if (n < K_NTYPE + 1) { for (k = 1; k < 8; k++) t->p[P_E0 + k] = ENG_KIT.edit[k].def; t->p[P_E7] = 0; }
        len = hit(t, &peak, &zc);
        if (len >= 20000) { bad++; printf("never ends: ty=%d dc=%d\n", t->p[P_E0], t->p[P_E2]); }
        if (len > worst_len) worst_len = len;
        if (peak > worst_peak) worst_peak = peak;
        if (peak > 32767) { bad++; if (bad < 5) printf("peak %d > 32767 ty=%d\n", peak, t->p[P_E0]); }
        if (n < K_NTYPE + 1) zc_ty[n] = zc;
    }
    for (k = 0; k < K_NTYPE + 1; k++) printf("type %s default: zero-crossings %ld\n", N_KIT_TYPE[k], zc_ty[k]);
    /* TYPE = KIT: 8 voices, one per GM drum at once; a closed hat chokes the open one; keys map */
    {
        static const uint8_t GM[8] = {36, 38, 46, 49, 51, 56, 41, 75};
        int32_t out[CTL]; int b, j, alive = 0, e0;
        t->p[P_E0] = K_KITMODE;
        for (k = 1; k < 8; k++) t->p[P_E0 + k] = ENG_KIT.desc(t, k) ? ENG_KIT.desc(t, k)->def : 0;
        t->p[P_E7] = 0;
        memset(t->v, 0, sizeof t->v);
        for (j = 0; j < 8; j++) { voice_t *v = &t->v[j]; v->note = GM[j]; v->active = 1; v->stage = 1; v->age = j + 1; ENG_KIT.note_on(t, v); }
        for (b = 0; b < 200; b++) for (j = 0; j < 8; j++) {
            vmod_t m; memset(&m, 0, sizeof m);
            if (t->v[j].active) { m.amp0 = m.amp1 = ENG_KIT.amp(t, &t->v[j], 1 << 15); if (!t->v[j].active) continue;
                memset(out, 0, sizeof out); ENG_KIT.render(t, &t->v[j], out, CTL, &m); }
        }
        for (j = 0; j < 8; j++) alive += t->v[j].active;
        printf("GM kit: %d of 8 voices still sounding after 0.15 s\n", alive);
        memset(t->v, 0, sizeof t->v);                              /* the choke: OH (46) running, CH (42) struck */
        t->v[0].note = 46; t->v[0].active = 1; t->v[0].stage = 1; ENG_KIT.note_on(t, &t->v[0]);
        t->v[1].note = 42; t->v[1].active = 1; t->v[1].stage = 1; t->v[1].age = 2; ENG_KIT.note_on(t, &t->v[1]);
        if (t->v[0].active || !t->v[1].active) { bad++; printf("choke failed\n"); } else printf("choke: CH cut the OH\n");
        e0 = ENG_KIT.keys(t, 0) * 1000 + ENG_KIT.keys(t, 5); if (e0 != 36046) { bad++; printf("keys map wrong %d\n", e0); }
        t->p[P_E0] = K_BD; if (ENG_KIT.keys(t, 0) != -1) { bad++; printf("keys must be -1 on a fixed type\n"); }
    }
    /* the mixer: a lane at 0 is silent, at 64 about 1/4 of the power (square law) */
    {
        int32_t pk100, pk0, i, nn; voice_t *v = &t->v[0];
        (void)i; (void)nn;
        for (k = 1; k < 8; k++) t->p[P_E0 + k] = ENG_KIT.edit[k].def;
        t->p[P_E0] = K_BD; t->p[P_E7] = 0;
        lane_lv = 127; (void)hit(t, &pk100, &i);
        lane_lv = 0; (void)hit(t, &pk0, &i);
        lane_lv = 127; (void)v;
        printf("mixer: BD peak %d at 100%%, %d at 0%%\n", pk100, pk0);
        if (pk0 != 0 || pk100 < 1000) { bad++; printf("mixer lane level failed\n"); }
    }
    /* per-lane sound in KIT mode: DECAY changes the length of a note of that lane, TUNE its pitch */
    {
        int32_t pk, zc1, zc2, n1, n2;
        t->p[P_E0] = K_KITMODE; for (k = 1; k < 8; k++) t->p[P_E0 + k] = ENG_KIT.desc(t, k) ? ENG_KIT.desc(t, k)->def : 0;
        t->p[P_E7] = 0; fixed_note = 36;
        snd_dc = 20; n1 = hit(t, &pk, &zc1);
        snd_dc = 100; n2 = hit(t, &pk, &zc1);
        snd_dc = 50;
        snd_tune = 0; (void)hit(t, &pk, &zc1);
        snd_tune = 24; (void)hit(t, &pk, &zc2); snd_tune = 0; fixed_note = -1;
        printf("lane sound: decay 20 -> %d blocks, 100 -> %d; zero crossings %d -> %d at TUNE +24\n", n1, n2, zc1, zc2);
        if (n2 <= n1 || zc2 <= zc1) { bad++; printf("lane sound failed\n"); }
    }
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
