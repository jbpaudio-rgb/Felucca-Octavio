/* SPDX-License-Identifier: MIT */
/* KIT: a synthesised drum voice, twelve types (BD SD CH OH CP RIM HTOM LTOM CRSH RIDE COWB CLAV), fixed point only.
 * Ported from the browser prototype's DrumEngine (fm1-engine-lab). One hit per voice (oneshot):
 * the engine's own envelope ends the voice through the amp hook.
 *
 * EDIT: TYPE TUNE DECAY PENV | TONE DRIVE SNAP HOLD
 *   TUNE  +-24 semitones around the type's base note   DECAY  tail length (8x range, exponential)
 *   PENV  pitch-envelope depth (0..127 = 0..48 st)     TONE   tone <-> noise mix (SD, hats, CP)
 *   DRIVE soft-clip gain                               SNAP   noise click at the attack
 *   HOLD  blocks the level is held before it decays (a gated-reverb tail)
 *
 * State: s[0] level (Q24), s[1] pitch env (Q15), s[2] noise generator, s[3] hold blocks left,
 * s[4] / s[6] one-pole states, s[5] blocks since the hit, s[7] snap env (Q15); ph[0..2] oscillators. */
static const char *const N_KIT_TYPE[] = {"BD", "SD", "CH", "OH", "CP", "RIM", "HTOM", "LTOM", "CRSH", "RIDE", "COWB", "CLAV"};
enum { K_BD, K_SD, K_CH, K_OH, K_CP, K_RIM, K_HTOM, K_LTOM, K_CRASH, K_RIDE, K_COW, K_CLAVE, K_NTYPE };
#define KIT_METAL(ty) ((ty) == K_CH || (ty) == K_OH || (ty) == K_CRASH || (ty) == K_RIDE)   /* the five-square voices */

static const uint8_t KIT_BASE[K_NTYPE] = {33, 50, 56, 56, 60, 76, 55, 45, 54, 60, 72, 100};      /* base MIDI note */
static const uint8_t KIT_TAU[K_NTYPE] = {40, 24, 6, 60, 20, 4, 36, 36, 60, 60, 12, 6};         /* blocks at DECAY 0 */
static const uint8_t KIT_PE[K_NTYPE] = {24, 8, 0, 0, 0, 4, 12, 12, 0, 0, 0, 2};            /* default pitch drop scale, 1/8 */

static void kit_note_on(struct track *t, voice_t *v)
{
    uint32_t seed = ((uint32_t)v->note * 2654435761u) ^ (v->age * 40503u) ^ 0x9E3779B9u;
    v->s[0] = 1 << 24;
    v->s[1] = KIT_METAL(clamp(t->p[P_E0], 0, K_NTYPE - 1)) ? 0 : 32767;
    v->s[2] = (int32_t)(seed ? seed : 1u);
    v->s[3] = ((int32_t)t->p[P_E7] * 40) / 127;
    v->s[4] = v->s[5] = v->s[6] = 0;
    v->s[7] = 32767;
    v->ph[0] = v->ph[1] = v->ph[2] = 0;
}

static int32_t kit_amp(struct track *t, voice_t *v, int32_t adsr)
{
    int32_t ty = clamp(t->p[P_E0], 0, K_NTYPE - 1), dc = clamp(t->p[P_E2], 0, 127), tau, a;
    (void)adsr;
    if (ty == K_CP)
        v->s[5]++;                                       /* (CH / OH keep s[5] as an oscillator phase) */
    v->s[7] -= v->s[7] >> 2;
    if (!KIT_METAL(ty))
        v->s[1] -= v->s[1] >> 3;                         /* pitch envelope (hats use s[1] as a filter state) */
    if (v->s[3] > 0) {
        v->s[3]--;
    } else {
        tau = (int32_t)KIT_TAU[ty] * (1 << (dc >> 5)) * (32 + (dc & 31)) >> 5;
        tau = FE_T(tau < 2 ? 2 : tau);
        v->s[0] -= mulq16(v->s[0], (uint32_t)(65536 / tau));
    }
    if (v->s[0] < (1 << 24) / 2000) {                    /* below -66 dB: the hit is over */
        v->s[0] = 0;
        v->active = 0;
        v->stage = 0;
        return 0;
    }
    a = v->s[0] >> 9;
    if (ty == K_CP && v->s[5] < 36)                      /* three claps, then the tail */
        a = mulq15(a, 32767 - (v->s[5] % 12) * 2400);
    return a;
}

static void kit_render(struct track *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    int32_t ty = clamp(t->p[P_E0], 0, K_NTYPE - 1), tone = clamp(t->p[P_E4], 0, 127), drive = clamp(t->p[P_E5], 0, 127);
    int32_t snap = clamp(t->p[P_E6], 0, 127), tune = (clamp(t->p[P_E1], -64, 63) * 24 * 16) / 64;
    int32_t pe = (((v->s[1] * clamp(t->p[P_E3], 0, 127)) >> 7) * (KIT_PE[ty] * 768 / 24)) >> 15;
    int32_t p16 = (int32_t)KIT_BASE[ty] * 16 + tune + pe, gain = 32768 + drive * 2048, sng;
    uint32_t inc = pitch_inc((uint32_t)clamp(p16, 0, 2047)), i;
    int32_t g_t = (127 - tone) << 8, g_n = tone << 8;
    int32_t *rng = &v->s[2];
    sng = mulq15(v->s[7], snap << 8);
    for (i = 0; i < n; i++) {
        int32_t y, nz = (int32_t)(noise32(rng) >> 16) - 32768, lp, hp;
        v->ph[0] += inc;
        switch (ty) {
        case K_SD:
            lp = v->s[4] += mulq15(nz - v->s[4], 7000);
            hp = nz - lp;
            y = mulq15(sine_i(v->ph[0]), g_t) + mulq15(hp, g_n);
            break;
        case K_CH:
        case K_OH:
        case K_CRASH:
        case K_RIDE: {                                   /* five inharmonic band-limited squares (808 ratios), 2 x HP */
            uint32_t p5 = (uint32_t)v->s[5], p6 = (uint32_t)v->s[6];
            int32_t hp1;
            v->ph[1] += (inc >> 8) * 380u;
            v->ph[2] += (inc >> 8) * 461u;
            p5 += (inc >> 8) * 652u;
            p6 += (inc >> 8) * 998u;
            y = osc_pulse(v->ph[0], inc, 0x80000000u) + osc_pulse(v->ph[1], (inc >> 8) * 380u, 0x80000000u) +
                osc_pulse(v->ph[2], (inc >> 8) * 461u, 0x80000000u) + osc_pulse(p5, (inc >> 8) * 652u, 0x80000000u) +
                osc_pulse(p6, (inc >> 8) * 998u, 0x80000000u);
            v->s[5] = (int32_t)p5;
            v->s[6] = (int32_t)p6;
            y = mulq15(y >> 2, g_t) + mulq15(nz, g_n);
            lp = v->s[4] += mulq15(y - v->s[4], 14000);   /* 12 dB/oct high-pass near 5 kHz */
            hp1 = y - lp;
            lp = v->s[1] += mulq15(hp1 - v->s[1], 14000);  /* second pole (s[1]: hats have no pitch envelope) */
            y = (hp1 - lp) * 3;                           /* the high-pass takes most of the level: make it up */
            break;
        }
        case K_CP:
            lp = v->s[4] += mulq15(nz - v->s[4], 7000);
            hp = nz - lp;
            lp = v->s[6] += mulq15(hp - v->s[6], 24000);
            y = lp + (lp >> 1);
            break;
        case K_COW:                                      /* two squares a fifth-ish apart (540 / 800 Hz), high-passed */
            v->ph[1] += (inc >> 8) * 379u;
            y = (osc_pulse(v->ph[0], inc, 0x80000000u) + osc_pulse(v->ph[1], (inc >> 8) * 379u, 0x80000000u)) >> 1;
            lp = v->s[4] += mulq15(y - v->s[4], 5000);
            y = mulq15(y - lp, 26000);
            break;
        case K_CLAVE:
            y = mulq15(sine_i(v->ph[0]), 24000);
            break;
        case K_RIM:
            y = mulq15(sine_i(v->ph[0]), 22000) + (mulq15(nz, 6000));
            break;
        default:                                         /* BD, HTOM, LTOM */
            y = sine_i(v->ph[0]);
            break;
        }
        y += mulq15(nz, sng) >> 1;
        y = softclip(mul_wrap(clamp(y, -32767, 32767), gain >> 4) >> 11);
        out[i] += voice_amp(y, m, i);
    }
}

#define KP(nm, ty, tn, dc, pe, to, dr, sn, ho) \
    {nm, {ty, tn, dc, pe, to, dr, sn, ho}, {0, 60, 127, 60}, 0, 0, FX(0, 0, 0, 0), PAT(0)}
static const preset_t KIT_PRESETS[] = {
    KP("BD", K_BD, 0, 50, 80, 0, 20, 30, 0),   KP("SD", K_SD, 0, 40, 20, 70, 10, 20, 0),
    KP("CH", K_CH, 0, 20, 0, 20, 0, 0, 0),     KP("OH", K_OH, 0, 70, 0, 20, 0, 0, 0),
    KP("CP", K_CP, 0, 50, 0, 0, 0, 0, 0),      KP("RIM", K_RIM, 0, 20, 20, 0, 0, 30, 0),
    KP("HTOM", K_HTOM, 0, 55, 60, 0, 0, 10, 0), KP("LTOM", K_LTOM, 0, 60, 60, 0, 0, 10, 0),
    KP("CRASH", K_CRASH, 0, 70, 0, 45, 0, 0, 0), KP("RIDE", K_RIDE, 0, 55, 0, 12, 0, 0, 0),
    KP("COWBELL", K_COW, 0, 50, 0, 0, 0, 0, 0), KP("CLAVES", K_CLAVE, 0, 30, 20, 0, 0, 0, 0),
};

static const engine_t ENG_KIT = {
    .name = "KIT",
    .page_title = {"VOICE", "SHAPE"},
    .edit = {
        {"TYPE", F_ENUM, 0, K_NTYPE - 1, 0, N_KIT_TYPE, 0},
        {"TUNE", F_BIPCT, -64, 63, 0, 0, 0},
        {"DECAY", F_PCT, 0, 127, 64, 0, 0},
        {"PENV", F_PCT, 0, 127, 64, 0, 0},
        {"TONE", F_PCT, 0, 127, 64, 0, 0},
        {"DRIVE", F_PCT, 0, 127, 0, 0, 0},
        {"SNAP", F_PCT, 0, 127, 0, 0, 0},
        {"HOLD", F_PCT, 0, 127, 0, 0, 0},
    },
    .presets = KIT_PRESETS,
    .npresets = NELEM(KIT_PRESETS),
    .note_on = kit_note_on,
    .render = kit_render,
    .knob = {P_E1, P_E2, P_E4, P_E5},
    .oneshot = 1,
    .amp = kit_amp,
};
