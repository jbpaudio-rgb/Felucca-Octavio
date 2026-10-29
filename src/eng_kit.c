/* SPDX-License-Identifier: MIT */
/* KIT: a synthesised drum voice, twelve drums (BD SD CH OH CP RIM HTOM LTOM CRSH RIDE COWB CLAV) and TYPE = KIT: the note picks the drum (GM map), fixed point only.
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
/* The mixer: six lanes. The host may provide KIT_LANE_LEVEL(t, lane) (0..127, 127 = as designed, square law) from
 * parameters of its own; without it every lane plays at 100 %. Lanes: 0 BD, 1 SD + RIM, 2 CP, 3 CH + OH,
 * 4 HTOM + LTOM, 5 CRSH + RIDE + COWB + CLAV */
/* Each lane's own sound in KIT mode: the host may provide KIT_LANE_SND(t, lane, k, def), k = 0 TUNE, 1 DECAY, 2 TONE,
 * 3 PENV (as the EDIT knobs of a fixed TYPE, def when the host has none) */
#ifndef KIT_LANE_SND
#define KIT_LANE_SND(t, lane, k, def) (def)
#endif
#ifndef KIT_LANE_LEVEL
#define KIT_LANE_LEVEL(t, lane) 127
#endif
static const uint8_t KIT_LANE[12] = {0, 1, 3, 3, 2, 1, 4, 4, 5, 5, 5, 5};   /* by type: BD SD CH OH CP RIM HTOM LTOM CRSH RIDE COWB CLAV */

static const char *const N_KIT_TYPE[] = {"BD", "SD", "CH", "OH", "CP", "RIM", "HTOM", "LTOM", "CRSH", "RIDE", "COWB", "CLAV", "KIT"};
enum { K_BD, K_SD, K_CH, K_OH, K_CP, K_RIM, K_HTOM, K_LTOM, K_CRASH, K_RIDE, K_COW, K_CLAVE, K_NTYPE, K_KITMODE = K_NTYPE };
#define KIT_METAL(ty) ((ty) == K_CH || (ty) == K_OH || (ty) == K_CRASH || (ty) == K_RIDE)   /* the five-square voices */

static const uint8_t KIT_BASE[K_NTYPE] = {33, 50, 56, 56, 60, 76, 55, 45, 54, 60, 72, 100};      /* base MIDI note */
static const uint8_t KIT_TAU[K_NTYPE] = {40, 24, 6, 60, 20, 4, 36, 36, 60, 60, 12, 6};         /* blocks at DECAY 0 */
static const uint8_t KIT_PE[K_NTYPE] = {24, 8, 0, 0, 0, 4, 12, 12, 0, 0, 0, 2};            /* default pitch drop scale, 1/8 */

/* TYPE = KIT: the note picks the drum, as on a General MIDI drum machine, so one part plays up to 8 drums at once.
 * GM notes 35..81 -> {type, semitones from the type's designed pitch}; other notes: their octave of 36..47 */
static const int8_t KIT_GM[47][2] = {
    {K_BD, 0}, {K_BD, 0}, {K_RIM, 0}, {K_SD, 0}, {K_CP, 0}, {K_SD, 2},                  /* 35 36 37 38 39 40 */
    {K_LTOM, -4}, {K_CH, 0}, {K_LTOM, -2}, {K_CH, 0}, {K_LTOM, 0}, {K_OH, 0},          /* 41 .. 46 */
    {K_HTOM, -7}, {K_HTOM, -5}, {K_CRASH, 0}, {K_HTOM, -2}, {K_RIDE, 0}, {K_CRASH, 2}, /* 47 .. 52 */
    {K_RIDE, 2}, {K_CH, 2}, {K_CRASH, 5}, {K_COW, 0}, {K_CRASH, -3}, {K_RIDE, -2},     /* 53 .. 58 */
    {K_RIDE, 4}, {K_CLAVE, -4}, {K_CLAVE, -2}, {K_CLAVE, 0}, {K_CLAVE, 2}, {K_CLAVE, 4}, /* 59 .. 64 */
    {K_HTOM, 4}, {K_HTOM, 2}, {K_LTOM, 4}, {K_LTOM, 2}, {K_COW, -4}, {K_COW, 4},       /* 65 .. 70 */
    {K_CH, -2}, {K_CH, -4}, {K_RIM, 2}, {K_RIM, -2}, {K_CLAVE, 6}, {K_CLAVE, -6},      /* 71 .. 76 */
    {K_CLAVE, 8}, {K_COW, 2}, {K_RIM, 4}, {K_RIM, -4}, {K_COW, -2},                    /* 77 .. 81 */
};
/* TYPE = KIT: six lanes (the mixer's), each playing the drum its EDIT knob picks (E1..E6 = lanes 1..6, E7 = DRIVE).
 * Lane k plays the notes of its group; while it keeps its own drum (LANE_DEF) the note decides the variant (a GM
 * open hat on the hats' lane), once the drum is changed the lane plays that drum for all its notes */
static const uint8_t KIT_LANE_DEF[6] = {K_BD, K_SD, K_CP, K_CH, K_HTOM, K_CRASH};
static const param_desc_t KIT_LT_DESC[6] = {
    {"LN1", F_ENUM, 0, K_NTYPE - 1, K_BD, N_KIT_TYPE, 0}, {"LN2", F_ENUM, 0, K_NTYPE - 1, K_SD, N_KIT_TYPE, 0},
    {"LN3", F_ENUM, 0, K_NTYPE - 1, K_CP, N_KIT_TYPE, 0}, {"LN4", F_ENUM, 0, K_NTYPE - 1, K_CH, N_KIT_TYPE, 0},
    {"LN5", F_ENUM, 0, K_NTYPE - 1, K_HTOM, N_KIT_TYPE, 0}, {"LN6", F_ENUM, 0, K_NTYPE - 1, K_CRASH, N_KIT_TYPE, 0},
};
static const param_desc_t KIT_DRV_DESC = {"DRIVE", F_PCT, 0, 127, 0, 0, 0};
static const param_desc_t *kit_desc(const struct track *t, uint32_t k)
{
    if (clamp(t->p[P_E0], 0, K_KITMODE) != K_KITMODE)
        return 0;
    return k >= 1u && k <= 6u ? &KIT_LT_DESC[k - 1u] : k == 7u ? &KIT_DRV_DESC : 0;
}
typedef struct { int32_t tune, dc, pe, tone, drive, snap, hold; } kit_p_t;
static void kit_pp(const struct track *t, int32_t lane, kit_p_t *q)        /* the sound's values: the knobs, or fixed ones in KIT mode */
{
    if (clamp(t->p[P_E0], 0, K_KITMODE) == K_KITMODE) {
        q->tune = clamp(KIT_LANE_SND(t, lane, 0, 0), -64, 63); q->dc = clamp(KIT_LANE_SND(t, lane, 1, 50), 0, 127);
        q->pe = clamp(KIT_LANE_SND(t, lane, 3, 64), 0, 127); q->tone = clamp(KIT_LANE_SND(t, lane, 2, 30), 0, 127); q->drive = clamp(t->p[P_E7], 0, 127); q->snap = 10; q->hold = 0;
        return;
    }
    q->tune = clamp(t->p[P_E1], -64, 63); q->dc = clamp(t->p[P_E2], 0, 127); q->pe = clamp(t->p[P_E3], 0, 127);
    q->tone = clamp(t->p[P_E4], 0, 127); q->drive = clamp(t->p[P_E5], 0, 127); q->snap = clamp(t->p[P_E6], 0, 127);
    q->hold = clamp(t->p[P_E7], 0, 127);
}

/* the drum a voice plays and its tuning offset (semitones) */
static int32_t kit_lane_of(const struct track *t, const voice_t *v)   /* the lane (mixer strip) a voice plays on */
{
    int32_t ty = clamp(t->p[P_E0], 0, K_KITMODE), n = v->note;
    if (ty < K_KITMODE)
        return KIT_LANE[ty];
    return KIT_LANE[n >= 35 && n <= 81 ? KIT_GM[n - 35][0] : n % 12];
}
static int32_t kit_ty(const struct track *t, const voice_t *v, int32_t *semi)
{
    int32_t ty = clamp(t->p[P_E0], 0, K_KITMODE), n = v->note, nt, lane, sel;
    *semi = 0;
    if (ty < K_KITMODE)
        return ty;
    if (n >= 35 && n <= 81) {
        *semi = KIT_GM[n - 35][1];
        nt = KIT_GM[n - 35][0];
    } else {
        nt = n % 12;                                     /* the 12 types over the octave */
    }
    lane = KIT_LANE[nt];
    sel = clamp(t->p[P_E1 + lane], 0, K_NTYPE - 1);
    return sel == KIT_LANE_DEF[lane] ? nt : sel;
}

/* the keys, when TYPE = KIT: key k plays the drum k % 12 of this order (the notes: BD RIM SD CP CH OH LTOM HTOM CRSH RIDE COWB CLAV) */
static int32_t kit_keys(const struct track *t, uint32_t k)
{
    static const uint8_t KEYN[12] = {36, 37, 38, 39, 42, 46, 43, 50, 49, 51, 56, 75};
    return clamp(t->p[P_E0], 0, K_KITMODE) == K_KITMODE ? (int32_t)KEYN[k % 12u] : -1;
}

static void kit_note_on(struct track *t, voice_t *v)
{
    int32_t semi, j;
    kit_p_t q;
    uint32_t seed = ((uint32_t)v->note * 2654435761u) ^ (v->age * 40503u) ^ 0x9E3779B9u;
    v->s[0] = 1 << 24;
    j = kit_ty(t, v, &semi);
    v->s[1] = KIT_METAL(j) ? 0 : 32767;
    if (j == K_CH && t->p[P_E0] == K_KITMODE) {          /* a closed hat chokes the open one */
        int32_t k;
        for (k = 0; k < NVOICE; k++) {
            voice_t *o = &t->v[k];
            int32_t os;
            if (o != v && o->active && kit_ty(t, o, &os) == K_OH) {
                o->s[0] = 0;
                o->active = 0;
                o->stage = 0;
            }
        }
    }
    v->s[2] = (int32_t)(seed ? seed : 1u);
    kit_pp(t, kit_lane_of(t, v), &q);
    v->s[3] = (q.hold * 40) / 127;
    v->s[4] = v->s[5] = v->s[6] = 0;
    v->s[7] = 32767;
    v->ph[0] = v->ph[1] = v->ph[2] = 0;
}

static int32_t kit_amp(struct track *t, voice_t *v, int32_t adsr)
{
    int32_t semi, ty = kit_ty(t, v, &semi), dc, tau, a;
    kit_p_t q;
    kit_pp(t, kit_lane_of(t, v), &q);
    dc = q.dc;
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
    kit_p_t q;
    int32_t semi, ty = kit_ty(t, v, &semi), tone, drive, snap, tune, pe;
    kit_pp(t, kit_lane_of(t, v), &q);
    tone = q.tone; drive = q.drive; snap = q.snap; tune = (q.tune * 24 * 16) / 64;
    pe = (((v->s[1] * q.pe) >> 7) * (KIT_PE[ty] * 768 / 24)) >> 15;
    int32_t p16 = (int32_t)KIT_BASE[ty] * 16 + semi * 16 + tune + pe, gain = 32768 + drive * 2048, sng;
    uint32_t inc = pitch_inc((uint32_t)clamp(p16, 0, 2047)), i;
    int32_t g_t = (127 - tone) << 8, g_n = tone << 8;
    int32_t *rng = &v->s[2];
    int32_t lv = clamp(KIT_LANE_LEVEL(t, KIT_LANE[ty]), 0, 127), lane_g = (lv * lv * 32768) / (127 * 127);
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
        y = mulq15(y, lane_g);
        out[i] += voice_amp(y, m, i);
    }
}

#define KP(nm, ty, tn, dc, pe, to, dr, sn, ho) \
    {nm, {ty, tn, dc, pe, to, dr, sn, ho}, {0, 60, 127, 60}, 0, 0, FX(0, 0, 0, 0), PAT(0)}
static const preset_t KIT_PRESETS[] = {
    KP("GM KIT", K_KITMODE, K_BD, K_SD, K_CP, K_CH, K_HTOM, K_CRASH, 0),
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
        {"TYPE", F_ENUM, 0, K_KITMODE, K_KITMODE, N_KIT_TYPE, 0},
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
    .keys = kit_keys,
    .desc = kit_desc,
};
