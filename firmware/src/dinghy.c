/* SPDX-License-Identifier: GPL-3.0-only */
/* Drumwelt: percussion found in nature, on the M-VAVE FM-1. Built on Dinghy (zvenson), by way of Groundkeeper.
 * An OBJECT is one drum sound: a rock (bass drums, toms), a stick (woodblocks, pipes) or a pile of leaves and seeds
 * (shakers). REC finds one at random, SAVE keeps it in the KIT (8 places). The keys play the object in hand, tuned to
 * the spell's scale; each object also has a cycle of its own (a Euclidean rhythm), and PLAY runs them all.
 *
 *   keys       play the object in hand, in the scale; F#5: the object silent (its hits to 0), again: back
 *              on ARP the white keys set the hits (1..16) and show them; the other black keys wait for a job
 *   REC        a tap finds a new object of the kind PRESETS chose (HOME, top left); held: the one before it comes back
 *   SAVE       a tap keeps the object in hand in the kit; held on a kit object: out of the kit, back in hand
 *   ALGORITHM  round the kit's objects (and the one in hand)
 *   SELECT     the spell; PRESETS what REC finds: rocks, sticks, leaves, plants; OCT- OCT+ the octave; PLAY the cycles
 *   HOME       KNOB 1..4: SIZE, MATTER, HOLLOW, STRIKE (the object's sound)
 *   pages      ENV attack, hold, decay, level; ARP steps, hits, rotate, rate (again: chance, wander); FX filter, res,
 *              space, mix; GLO tempo, pulse, chords, tonic; SEL season, roots, shape, scale; LFO the object's own:
 *              moves (which setting), every (beats), depth, wave. A plant's HOME knobs: STALK, SAP, BEND, QUIVER
 *   MIDI in    notes from USB or the TRS jack play the object in hand, with their velocity
 *
 * Rules of the road (the chip has no FPU and no libgcc): integers only, no float, no 64-bit division.
 * tests/host.c runs this file on a computer: the screen as a PNG, the sound as a WAV.
 * What platform.c offers: docs/API.md. The chip's rules: docs/RULES.md. */
#define APP_NAME "DRUMWELT"                    /* the title, the rescue and the crash screen */
#define CV_MAX (240u * 120u)                    /* the object's picture: one canvas */
static void app_idle(void);
#define LCD_IDLE app_idle                       /* while the screen's DMA runs: MIDI in, the cycles (platform.c) */
#include "platform.c"
#include "dw_tables.h"

/* ---- the sound (runs in the audio ISR: keep it short, no flash, no screen)
 * A voice is one struck object: three sine partials (the body: a fundamental and two overtones, each with its own
 * level and decay, all falling in pitch from the blow) and a noise through a band-pass (the blow itself; for leaves and
 * seeds the whole sound, broken into grains). The main loop works out the numbers for a hit (strike) and hands them
 * over; here the envelopes move every CH frames and the gains ramp between, so a hit starts within a third of a
 * millisecond without a click. */
#define NV 8u                                   /* voices */
#define CH 16u                                  /* frames a chunk: the envelopes' step (DK counts in these) */
typedef struct {
    uint32_t inc;                               /* the fundamental's phase step: f * 2^32 / 44100 */
    uint32_t r2, r3;                            /* the overtones' ratios, Q12 */
    int32_t lv[3];                              /* the partials' levels at the blow, Q15 */
    uint32_t dk[3];                             /* ... and what each keeps a chunk, Q16 (65536: all, it follows the envelope) */
    int32_t drop;                               /* the pitch at the blow: this much above (Q12), falling by drop_k */
    uint32_t drop_k;
    int32_t nlv, nf, nq, nhp;                   /* the noise: level Q15; the band's 2 sin(pi f / FS) and 1/Q, Q15; high-pass share Q8 */
    uint32_t ndk;
    uint32_t gp, gpk;                           /* grains: the chance of one a frame (of 65536; 0: no grains), thinning by gpk */
    int32_t gfall;                              /* ... and how fast a grain dies, Q15 a frame */
    int32_t peak;                               /* the envelope's top, Q15 */
    uint32_t att, hold, mdk;                    /* attack and hold in chunks; the decay, Q16 a chunk */
    uint32_t fm;                                /* plants: partial 2 is no overtone but bends the phase of 1 and 3 (FM); its level is the depth */
    uint32_t rise;                              /* the pitch at the blow lies under the note (drop: how far), not over */
    uint32_t trinc, trdep;                      /* a quiver in the level: its step a chunk; its depth, Q16 */
} vpar_t;
static struct {
    vpar_t nx;                                  /* the next hit (main loop writes, then trig) */
    volatile uint32_t trig;                     /* +1 = strike now */
    uint32_t tseen;
    vpar_t p;                                   /* the hit sounding (audio's copy) */
    uint32_t ph[3], rng, left, gpn, tph;
    int32_t e[3], pe, ne, a, astep;             /* levels x256: the partials', the pitch fall, the noise's, the envelope */
    int32_t g[4];                               /* the gains at the end of the last chunk, Q15 */
    int32_t lp, bp, gr;                         /* the band-pass; the grain's level */
    volatile uint32_t stage;                    /* 1 attack, 2 hold, 3 decay, 0 silent */
    volatile uint32_t age;
    volatile uint8_t slot;                      /* whose hit it is (the kit's place) */
} voice[NV];

static int32_t sine(uint32_t ph)                /* a full cycle over 2^32 -> -32767..32767 (a refined parabola, ~0.1 %) */
{
    int32_t x = (int32_t)ph >> 16, ax = x < 0 ? -x : x, y, ay;
    y = (x * (32768 - ax)) >> 13;               /* 4x(1-|x|), Q15 */
    ay = y < 0 ? -y : y;
    y += ((((y * ay) >> 15) - y) * 7373) >> 15; /* + 0.225 (y|y| - y) */
    return y > 32767 ? 32767 : y < -32767 ? -32767 : y;
}
static uint32_t xs32(uint32_t *r) { uint32_t x = *r; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return *r = x; }
static uint32_t keep(uint32_t x, uint32_t k) { return (uint32_t)(((uint64_t)x * k) >> 16); }   /* x * k / 65536 */

static int voices_audio(int32_t *bus, uint32_t n)   /* adds the voices, mono, to bus; 1 if any sounds */
{
    uint32_t v, i, j, c0;
    int any = 0;
    for (v = 0; v < NV; v++) {
        vpar_t *p = &voice[v].p;
        if (voice[v].trig != voice[v].tseen) {  /* a new hit: its numbers, the envelopes from the top */
            voice[v].tseen = voice[v].trig;
            *p = voice[v].nx;
            if (voice[v].a < 2048) { voice[v].ph[0] = voice[v].ph[1] = voice[v].ph[2] = 0; voice[v].lp = voice[v].bp = 0; }
            for (j = 0; j < 3u; j++) voice[v].e[j] = p->lv[j] << 8;
            voice[v].pe = p->drop << 8;
            voice[v].ne = 32767 << 8;
            voice[v].gpn = p->gp << 12;
            voice[v].gr = p->gp ? 30000 : 32767;    /* (the first grain is the blow itself: on time) */
            voice[v].left = p->hold;
            voice[v].astep = p->att ? (p->peak << 8) / (int32_t)p->att : 0;
            voice[v].tph = 0;
            voice[v].stage = 1;
            if (!voice[v].rng) voice[v].rng = 0x9E3779B9u + v * 7919u;
        }
        if (!voice[v].stage && !(voice[v].g[0] | voice[v].g[1] | voice[v].g[2] | voice[v].g[3])) continue;   /* silent: costs nothing */
        any = 1;
        for (c0 = 0; c0 < n; c0 += CH) {
            uint32_t m = n - c0 < CH ? n - c0 : CH, inc[3], ph[3], act[4], rng = voice[v].rng, grains = p->gp != 0u, gp16, fm = p->fm;
            int32_t tg[4], g[4], d[4], am, a = voice[v].a, lp = voice[v].lp, bp = voice[v].bp, gr = voice[v].gr;
            int32_t nf = p->nf, nq = p->nq, nhp = p->nhp, gfall = p->gfall;
            if (voice[v].stage == 1u) {         /* the envelope: up, wait, down */
                if (!p->att) { a = p->peak << 8; voice[v].stage = 2; }
                else { a += voice[v].astep; if (a >= p->peak << 8) { a = p->peak << 8; voice[v].stage = 2; } }
            } else if (voice[v].stage == 2u) {
                if (voice[v].left) voice[v].left--; else voice[v].stage = 3;
            } else if (voice[v].stage == 3u) {
                a = (int32_t)keep((uint32_t)a, p->mdk);
                if (a < 1024) { a = 0; voice[v].stage = 0; }
            } else a = 0;
            voice[v].a = a;
            am = a >> 8;
            if (p->trdep) {                     /* the quiver: the level dips and comes back */
                int32_t w = 32767 - sine(voice[v].tph += p->trinc);   /* 0 .. 65534 */
                am -= (int32_t)(((int64_t)am * ((w * (int32_t)(p->trdep >> 1)) >> 15)) >> 16);
            }
            for (j = 0; j < 3u; j++) {          /* the partials: each its own decay under the envelope */
                if (p->dk[j] < 65536u) voice[v].e[j] = (int32_t)keep((uint32_t)voice[v].e[j], p->dk[j]);
                tg[j] = (am * (voice[v].e[j] >> 8)) >> 15;
            }
            if (fm) tg[1] = a ? voice[v].e[1] >> 8 : 0;   /* (a depth, not a level: it does not follow the envelope) */
            if (p->ndk < 65536u) voice[v].ne = (int32_t)keep((uint32_t)voice[v].ne, p->ndk);
            tg[3] = (am * ((((voice[v].ne >> 8) * p->nlv) >> 15))) >> 15;
            voice[v].pe = (int32_t)keep((uint32_t)voice[v].pe, p->drop_k);
            voice[v].gpn = keep(voice[v].gpn, p->gpk);
            gp16 = voice[v].gpn >> 12;
            inc[0] = (uint32_t)(((uint64_t)p->inc * (uint32_t)(voice[v].pe >> 8)) >> 12);
            inc[0] = p->rise ? p->inc - inc[0] : p->inc + inc[0];
            inc[1] = (uint32_t)(((uint64_t)inc[0] * p->r2) >> 12);
            inc[2] = (uint32_t)(((uint64_t)inc[0] * p->r3) >> 12);
            for (j = 0; j < 3u; j++) {
                uint64_t top = (uint64_t)inc[0] * (j == 0u ? 4096u : j == 1u ? p->r2 : p->r3);
                if (top >> 12 > 0x60000000u) tg[j] = 0;   /* (above ~16 kHz: left out, no aliasing) */
            }
            for (j = 0; j < 4u; j++) {
                g[j] = voice[v].g[j];
                d[j] = m == CH ? (tg[j] - g[j]) >> 4 : (tg[j] - g[j]) / (int32_t)m;
                if (!d[j]) g[j] = tg[j];        /* (the last small step, at once) */
                act[j] = (uint32_t)(g[j] | tg[j]);
                voice[v].g[j] = tg[j];
            }
            for (j = 0; j < 3u; j++) ph[j] = voice[v].ph[j];
            for (i = 0; i < m; i++) {
                int32_t s = 0;
                if (fm) {                       /* a plant: 2 bends the phase of 1 and 3 (at its top level, a full turn) */
                    uint32_t mod;
                    ph[1] += inc[1]; g[1] += d[1];
                    mod = (uint32_t)(sine(ph[1]) * g[1]) << 2;
                    if (act[0]) { ph[0] += inc[0]; g[0] += d[0]; s += (sine(ph[0] + mod) * g[0]) >> 15; }
                    if (act[2]) { ph[2] += inc[2]; g[2] += d[2]; s += (sine(ph[2] + mod) * g[2]) >> 15; }
                } else {
                    if (act[0]) { ph[0] += inc[0]; g[0] += d[0]; s += (sine(ph[0]) * g[0]) >> 15; }
                    if (act[1]) { ph[1] += inc[1]; g[1] += d[1]; s += (sine(ph[1]) * g[1]) >> 15; }
                    if (act[2]) { ph[2] += inc[2]; g[2] += d[2]; s += (sine(ph[2]) * g[2]) >> 15; }
                }
                if (act[3]) {
                    uint32_t r = xs32(&rng);
                    int32_t x = (int16_t)(r >> 16), hp, y;
                    if (grains) {               /* leaves and seeds: the noise in grains, each dying fast */
                        if ((r & 0xFFFFu) < gp16) { gr += 6000 + (int32_t)((r >> 3) & 0x3FFFu); if (gr > 32767) gr = 32767; }
                        x = (x * gr) >> 15;
                        gr -= ((gr * gfall) >> 15) + 1;
                        if (gr < 0) gr = 0;
                    }
                    lp += (int32_t)(((int64_t)nf * bp) >> 15);
                    hp = x - lp - (int32_t)(((int64_t)nq * bp) >> 15);
                    bp += (int32_t)(((int64_t)nf * hp) >> 15);
                    y = bp + ((hp * nhp) >> 8);
                    if (y > 60000) y = 60000; else if (y < -60000) y = -60000;
                    g[3] += d[3];
                    s += (y * g[3]) >> 15;
                }
                bus[c0 + i] += s;
            }
            for (j = 0; j < 3u; j++) voice[v].ph[j] = ph[j];
            voice[v].rng = rng; voice[v].lp = lp; voice[v].bp = bp; voice[v].gr = gr;
        }
    }
    return any;
}

/* ---- the sound around the voices (audio ISR): a resonant low-pass, then a reverb, then dry + wet out in stereo
 * (Groundkeeper's). FILTER: a state-variable low-pass (Chamberlin), run twice a sample so it stays stable up to
 * 12 kHz; its states carry 4 extra bits so the low cutoffs stay smooth. RES: its Q, with the input turned down as the
 * resonance rises. REVERB: four delay lines (50..95 ms) that feed back into each other through a Hadamard matrix, each
 * with a soft low-pass (a forest, not a tiled room), after two allpasses that blur the attack; L and R take different
 * lines. SPACE sets the feedback (a decay of 0.6 s .. 16 s), MIX dry against wet. The coefficients glide (no zipper
 * noise), and a soft limiter keeps a peak from clipping. The tail runs on ~20 s after the last hit, then rests. */
#ifdef DINGHY_HOST
#define POOL
#else
#define POOL __attribute__((section(".pool")))   /* (the pool: big buffers, zeroed at boot) */
#endif
/* (computed on a computer: no float on the chip) */
static const uint16_t CUT_F[25] = {187, 227, 275, 335, 406, 494, 600, 728, 885, 1074, 1305, 1585, 1925, 2338, 2839, 3448, 4187, 5084, 6172, 7491, 9090, 11023, 13359, 16171, 27167};   /* 2 sin(pi fc / 2FS), Q15: 80 Hz .. 7 kHz in 24 steps, then OPEN (12 kHz) */
static const char *const CUT_LABEL[25] = {"80", "100", "120", "140", "170", "210", "260", "310", "380", "460", "560", "680", "820", "1.0k", "1.2k", "1.5k", "1.8k", "2.2k", "2.6k", "3.2k", "3.9k", "4.7k", "5.8k", "7.0k", "OPEN"};
static const uint16_t RES_Q[16] = {46348, 40681, 35707, 31342, 27510, 24146, 21194, 18603, 16328, 14332, 12580, 11041, 9691, 8507, 7466, 6554};   /* 1/Q, Q15: Q 0.71 .. 5 */
static const uint16_t RES_COMP[16] = {32768, 31676, 30518, 29297, 28020, 26694, 25329, 23935, 22522, 21103, 19689, 18293, 16926, 15598, 14318, 13094};   /* input gain, Q15: the resonance's boost taken back */
static const uint16_t SPACE_G[16] = {14228, 16764, 19125, 21260, 23148, 24785, 26183, 27364, 28351, 29169, 29844, 30398, 30850, 31218, 31517, 31759};   /* feedback, Q15: a decay of 0.6 s .. 16 s */
static const char *const SPACE_LABEL[16] = {"0.6s", "0.7s", "0.9s", "1.2s", "1.4s", "1.8s", "2.2s", "2.8s", "3.5s", "4.3s", "5.4s", "6.7s", "8.3s", "10s", "13s", "16s"};

#define NCUT 25u
#define NRES 16u
#define NSPACE 16u
#define NMIX 21u                                /* 0 .. 100 % in 5 % steps */
static volatile uint32_t fx_cut = NCUT - 1u, fx_res, fx_space = 4, fx_mix = 3;   /* (main loop writes) */
static int32_t f_now = 27167, q_now = 46348, c_now = 32768, g_now, m_now;   /* the gliding coefficients */
static int32_t sv_lp, sv_bp;                    /* the filter's states (x16) */
#define RV0 2203u
#define RV1 2851u
#define RV2 3517u
#define RV3 4211u
#define AP0 142u
#define AP1 379u
static int16_t rv0[RV0] POOL, rv1[RV1] POOL, rv2[RV2] POOL, rv3[RV3] POOL, ap0[AP0] POOL, ap1[AP1] POOL;
static uint32_t ri0, ri1, ri2, ri3, ai0, ai1, rv_rest;
static int32_t dm0, dm1, dm2, dm3;              /* the lines' damping states */
#define RV_DAMP 18022                           /* the lines' low-pass, Q15: ~5.6 kHz */
#define RV_DIFF 20480                           /* the allpasses' gain, Q15: 0.625 */
#define RV_TAIL (44100u * 20u)                  /* frames the reverb runs on after its input goes quiet */
static int32_t sat16(int32_t x) { return x > 32767 ? 32767 : x < -32767 ? -32767 : x; }
static int32_t glide(int32_t now, int32_t to) { int32_t d = to - now; return d > -8 && d < 8 ? to : now + (d >> 3); }
static int32_t soft(int32_t x)                  /* above 3/4 of full scale, a quarter as steep */
{
    return x > 24576 ? 24576 + ((x - 24576) >> 2) : x < -24576 ? -24576 + ((x + 24576) >> 2) : x;
}

static void fx_audio(const int32_t *bus, int32_t *out, uint32_t n, int quiet_in)
{
    uint32_t i, k;
    int32_t f, q, c, g, m, dry;
    f_now = glide(f_now, CUT_F[fx_cut]);
    q_now = glide(q_now, RES_Q[fx_res]);
    c_now = glide(c_now, RES_COMP[fx_res]);
    g_now = glide(g_now, SPACE_G[fx_space]);
    m_now = glide(m_now, (int32_t)fx_mix * 1638);   /* 0 .. 32760 */
    if (quiet_in) {
        if (rv_rest >= RV_TAIL) { sv_lp = sv_bp = 0; return; }   /* all quiet: costs nothing */
        rv_rest += n;
    } else rv_rest = 0;
    f = f_now; q = q_now; c = c_now; g = g_now; m = m_now;
    dry = 32767 - (m >> 1);                     /* at MIX 100 % the dry is half */
    for (i = 0; i < n; i++) {
        int32_t x = (int32_t)(((int64_t)bus[i] * c) >> 15) << 4, y, l = 0, r = 0;
        for (k = 0; k < 2u; k++) {              /* the filter, twice a sample */
            int32_t hp;
            sv_lp += (int32_t)(((int64_t)f * sv_bp) >> 15);
            hp = x - sv_lp - (int32_t)(((int64_t)q * sv_bp) >> 15);
            sv_bp += (int32_t)(((int64_t)f * hp) >> 15);
        }
        y = sv_lp >> 4;
        if (m) {                                /* the reverb */
            int32_t in = y >> 2, v, t, y0, y1, y2, y3, a, b, cc, e;
            v = in - ((RV_DIFF * ap0[ai0]) >> 15); t = ap0[ai0] + ((RV_DIFF * v) >> 15); ap0[ai0] = (int16_t)sat16(v);
            if (++ai0 == AP0) ai0 = 0;
            v = t - ((RV_DIFF * ap1[ai1]) >> 15); in = ap1[ai1] + ((RV_DIFF * v) >> 15); ap1[ai1] = (int16_t)sat16(v);
            if (++ai1 == AP1) ai1 = 0;
            y0 = rv0[ri0]; y1 = rv1[ri1]; y2 = rv2[ri2]; y3 = rv3[ri3];
            dm0 += ((y0 - dm0) * RV_DAMP + 16384) >> 15; dm1 += ((y1 - dm1) * RV_DAMP + 16384) >> 15;   /* (rounded: */
            dm2 += ((y2 - dm2) * RV_DAMP + 16384) >> 15; dm3 += ((y3 - dm3) * RV_DAMP + 16384) >> 15;   /*  no drift) */
            a = dm0 + dm1; b = dm0 - dm1; cc = dm2 + dm3; e = dm2 - dm3;   /* Hadamard: (a+c, b+e, a-c, b-e) / 2 */
            rv0[ri0] = (int16_t)sat16(in + ((((b + e) >> 1) * g + 16384) >> 15));   /* (Hadamard, then rotated a */
            rv1[ri1] = (int16_t)sat16(-in + ((((a - cc) >> 1) * g + 16384) >> 15)); /*  line: a Hadamard alone */
            rv2[ri2] = (int16_t)sat16(in + ((((b - e) >> 1) * g + 16384) >> 15));   /*  swaps two patterns back */
            rv3[ri3] = (int16_t)sat16(-in + ((((a + cc) >> 1) * g + 16384) >> 15)); /*  and forth; this spreads) */
            if (++ri0 == RV0) ri0 = 0;
            if (++ri1 == RV1) ri1 = 0;
            if (++ri2 == RV2) ri2 = 0;
            if (++ri3 == RV3) ri3 = 0;
            l = ((y0 + y1 - y2 - y3) * (m >> 1)) >> 15;   /* L and R: two sign patterns across all four lines, */
            r = ((y0 - y1 - y2 + y3) * (m >> 1)) >> 15;   /* orthogonal to the input's: decorrelated */
        }
        y = (y * dry) >> 15;
        out[2u * i] += soft(y + l);
        out[2u * i + 1u] += soft(y + r);
    }
}

#define BUS_N 256u
static int32_t bus[BUS_N];
#ifndef DINGHY_HOST
static volatile uint32_t cpu_max_us;            /* the audio ISR's longest run since the main loop last looked */
#endif

static void app_audio(int32_t *out, uint32_t n)
{
    uint32_t i;
    int k;
#ifndef DINGHY_HOST
    uint32_t t0 = fm1_ticks(), us;
#endif
    if (n > BUS_N) n = BUS_N;
    for (i = 0; i < n; i++) bus[i] = 0;
    k = voices_audio(bus, n);
    fx_audio(bus, out, n, !k);
#ifndef DINGHY_HOST
    us = (fm1_ticks() - t0) / FM1_TICKS_PER_US;
    if (us > cpu_max_us) cpu_max_us = us;
#endif
}

/* ---- notes (main loop) */
static const uint32_t INC_C9[12] = {            /* MIDI 120..131 (C9..B9); lower octaves shift right */
    815363807u, 863847862u, 915214929u, 969636441u, 1027294024u, 1088380105u,
    1153098554u, 1221665363u, 1294309365u, 1371273005u, 1452813141u, 1539201906u};
static uint32_t note_inc(uint32_t note) { return INC_C9[note % 12u] >> (10u - note / 12u); }

/* ---- the spells (main loop; Groundkeeper's): the harmony, and how it moves.
 * A SCALE is a root and a mode. A SPELL is a group of scales that belong together, with a name. The season moves the
 * harmony between a spell's scales: WHEEL walks round the circle of fifths a step at a time; the other spells leap. A
 * move pivots: the new scale's chord that shares the most with the one sounding takes over; at the next pulse the
 * harmony goes home, to the new scale's first chord. Chords stack every other note of the scale; they come in a ring
 * that falls by a third, the tense half-diminished and diminished ones left out. The keys play in the scale; the
 * cycles play the chord's notes. */
#define NMODE 10u
enum { M_MAJ, M_DOR, M_PHR, M_LYD, M_MIX, M_MIN, M_HMIN, M_MMIN, M_PENT, M_MPENT };
static const uint8_t MODE_N[NMODE] = {7, 7, 7, 7, 7, 7, 7, 7, 5, 5};
static const uint8_t MODE_PC[NMODE][7] = {{0, 2, 4, 5, 7, 9, 11}, {0, 2, 3, 5, 7, 9, 10}, {0, 1, 3, 5, 7, 8, 10},
    {0, 2, 4, 6, 7, 9, 11}, {0, 2, 4, 5, 7, 9, 10}, {0, 2, 3, 5, 7, 8, 10}, {0, 2, 3, 5, 7, 8, 11}, {0, 2, 3, 5, 7, 9, 11},
    {0, 2, 4, 7, 9}, {0, 3, 5, 7, 10}};
static const char *const MODE_NAME[NMODE] = {"MAJOR", "DORIAN", "PHRYGIAN", "LYDIAN", "MIXOLYD", "MINOR", "HARM MIN",
    "MEL MIN", "PENTA", "MIN PENT"};
typedef struct { const char *name; uint8_t walk, n, off[12], mode[12]; } spell_t;   /* off: from the tonic, semitones */
#define NSPELL 6u
static const spell_t SPELL[NSPELL] = {
    {"WHEEL", 1, 12, {0, 7, 2, 9, 4, 11, 6, 1, 8, 3, 10, 5}, {0}},                       /* the twelve major keys, by fifths */
    {"STILLWATER", 0, 6, {0, 2, 4, 5, 7, 9}, {M_MAJ, M_DOR, M_PHR, M_LYD, M_MIX, M_MIN}},   /* one set of notes, six centres */
    {"HOLLOW", 0, 5, {0, 0, 0, 0, 0}, {M_MIN, M_DOR, M_HMIN, M_MMIN, M_PHR}},              /* the minors on one root */
    {"MEADOW", 0, 4, {0, 0, 0, 7}, {M_LYD, M_MAJ, M_MIX, M_LYD}},                           /* bright: lydian light */
    {"FAE RING", 0, 6, {0, 4, 8, 9, 1, 5}, {M_MAJ, M_MAJ, M_MAJ, M_MIN, M_MIN, M_MIN}},     /* keys a third apart */
    {"OLD SONG", 0, 4, {0, 7, 5, 0}, {M_PENT, M_PENT, M_PENT, M_MPENT}}};                   /* pentatonic, open */
static const uint8_t FIFTHS[12] = {0, 7, 2, 9, 4, 11, 6, 1, 8, 3, 10, 5};   /* the tonics round the circle of fifths */
static const char *const PC_NAME[12] = {"C", "Db", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};
static const char *const TONIC_NAME[12] = {"C", "G", "D", "A", "E", "B", "F#", "Db", "Ab", "Eb", "Bb", "F"};

static uint32_t grow_rng = 0x2545F491u, chord_now, moves_left, pulse_ms;
static uint32_t spell_now, tonic, scale_now;    /* the spell cast, its tonic (on the circle of fifths), its scale now */
static uint8_t scale_pc[7], ring[7];            /* the scale now: its notes; its chords' degrees, in ring order */
static uint32_t scale_n, ring_n;
static uint32_t chord_every = 5u;               /* CHORDS: pulses a chord (0: HOLD) */
static int homing;                              /* a new scale: at the next pulse, its home chord */
static volatile int32_t run;                    /* PLAY: the cycles run */

static uint32_t rnd(uint32_t n)                 /* 0 .. n-1 (xorshift32) */
{
    grow_rng ^= grow_rng << 13; grow_rng ^= grow_rng >> 17; grow_rng ^= grow_rng << 5;
    return n ? grow_rng % n : 0u;
}
static void scale_load(void)                    /* the scale now: its notes, and its ring of chords */
{
    const spell_t *sp = &SPELL[spell_now];
    uint32_t m = sp->mode[scale_now], root = (FIFTHS[tonic] + sp->off[scale_now]) % 12u, i, j;
    scale_n = MODE_N[m];
    for (i = 0; i < scale_n; i++) scale_pc[i] = (uint8_t)((root + MODE_PC[m][i]) % 12u);
    ring_n = 0;
    for (j = 0; j < scale_n; j++) {             /* falling by thirds: 0, n-2, n-4, ... */
        uint32_t d = (scale_n * 2u - 2u * j) % scale_n, k, seen = 0, t, f;
        for (k = 0; k < ring_n; k++) seen |= ring[k] == d;
        if (seen) break;
        t = (scale_pc[(d + 2u) % scale_n] + 12u - scale_pc[d]) % 12u;
        f = (scale_pc[(d + 4u) % scale_n] + 12u - scale_pc[d]) % 12u;
        if ((t == 3u || t == 4u) && f == 6u) continue;   /* (half-diminished, diminished: left out) */
        ring[ring_n++] = (uint8_t)d;
    }
}
static uint32_t cpc(uint32_t c, uint32_t k)     /* chord c's k-th pitch class: 0 root, 1..3 its tones, 4 its colour */
{
    uint32_t d = ring[c % ring_n];
    return scale_pc[(d + (k < 4u ? 2u * k : 8u)) % scale_n];
}
static const char *chord_qual(uint32_t c)       /* maj7, m7, 7, ... from the chord's intervals ("" if it has no name) */
{
    static const struct { uint8_t t, f, s; const char *q; } Q[] = {{4, 7, 11, "maj7"}, {3, 7, 10, "m7"}, {4, 7, 10, "7"},
        {3, 7, 11, "mMaj7"}, {4, 8, 11, "maj7#5"}, {4, 9, 2, "6/9"}, {5, 10, 2, "9sus"}, {5, 10, 3, "7sus"}, {5, 9, 2, "6sus"},
        {3, 10, 5, "m11"}, {4, 7, 9, "6"}, {3, 7, 9, "m6"}};
    uint32_t r = cpc(c, 0), t = (cpc(c, 1) + 12u - r) % 12u, f = (cpc(c, 2) + 12u - r) % 12u, s = (cpc(c, 3) + 12u - r) % 12u, i;
    for (i = 0; i < sizeof Q / sizeof Q[0]; i++)
        if (Q[i].t == t && Q[i].f == f && Q[i].s == s) return Q[i].q;
    return "";
}
static uint32_t new_moves(void) { return chord_every < 3u ? chord_every : chord_every - 1u + rnd(3u); }   /* (a little give) */
static void cast(uint32_t sp, uint32_t tn, uint32_t sc)   /* to this spell, tonic and scale, pivoting on the chord sounding */
{
    uint32_t old[4], c, k, j, best = 0, bests = 0;
    for (k = 0; k < 4u; k++) old[k] = ring_n ? cpc(chord_now, k) : 0u;
    spell_now = sp; tonic = tn; scale_now = sc % SPELL[sp].n;
    scale_load();
    for (c = 0; c < ring_n; c++) {              /* the new scale's chord sharing the most with the one sounding: often the same */
        uint32_t s = 0;
        for (k = 0; k < 4u; k++) for (j = 0; j < 4u; j++) s += cpc(c, k) == old[j];
        s = s * 2u + (cpc(c, 0) == old[0]);     /* (the same root breaks a tie) */
        if (s > bests) { bests = s; best = c; }
    }
    chord_now = best;
    homing = 1;                                 /* (on the pivot until the next pulse; then home) */
}
static int32_t season_dir = 1;                  /* WHEEL: +1 travelling sharpwards round the circle, -1 flatwards */
static void season_turn(void)                   /* the season moves the harmony: WHEEL a step (mostly onward), the rest a leap */
{
    const spell_t *sp = &SPELL[spell_now];
    uint32_t to;
    if (sp->n < 2u) return;
    if (sp->walk) {
        if (!rnd(3u)) season_dir = -season_dir;
        to = (scale_now + (season_dir > 0 ? 1u : sp->n - 1u)) % sp->n;
    } else
        to = (scale_now + 1u + rnd(sp->n - 1u)) % sp->n;   /* any other of the spell's scales */
    cast(spell_now, tonic, to);
}

static int32_t roots_dir = 1;                   /* ROOTS: +1 sharpwards round the circle of fifths, -1 flatwards */
static void roots_turn(void)                    /* ROOTS: the tonic a fifth along (mostly onward), the scale kept, pivoting */
{
    if (!rnd(3u)) roots_dir = -roots_dir;
    cast(spell_now, (tonic + (roots_dir > 0 ? 1u : 11u)) % 12u, scale_now);
}
static void pulse_move(void)                    /* a pulse: now and then the chord moves to a neighbour on the ring */
{
    pulse_ms = fm1_ms;
    if (homing) {                               /* a new scale: home (also with CHORDS on HOLD) */
        homing = 0;
        chord_now = 0;
        moves_left = new_moves();
    } else if (chord_every && !moves_left) {
        chord_now = (chord_now + (rnd(3u) ? 1u : ring_n - 1u)) % ring_n;
        moves_left = new_moves();
    }
    if (moves_left) moves_left--;
#ifdef DINGHY_HOST
    printf("%7u ms  chord %s%s\n", (unsigned)fm1_ms, PC_NAME[cpc(chord_now, 0)], chord_qual(chord_now));
#endif
}

/* ---- time (main loop; Groundkeeper's): TEMPO in BPM; the PULSE and SEASON waves.
 * PULSE moves the chord, a move each time its wave rises through the middle; its length is in beats (1/4 .. 64).
 * SINE: regular; RANDOM: a smooth random wave, so the moves come irregularly; MIX: the two summed. SEASON and ROOTS
 * (OFF, 5 s .. 10 min) are slow random waves: each time one peaks or dips, the scale (or the tonic) turns. */
typedef struct { uint32_t ph; int32_t a, b; uint8_t seg, armed, zone; } lfo_t;   /* seg: log2 of random targets a cycle */
static lfo_t pulse = {0, 0, 0, 2, 0, 0}, season = {0, 0, 0, 1, 0, 0}, roots = {0, 0, 0, 1, 0, 0};
static uint32_t lfo_t0;
#define NPULSE 15u
static const uint16_t PULSE_Q[NPULSE] = {0, 1, 2, 4, 8, 12, 16, 24, 32, 48, 64, 96, 128, 192, 256};   /* quarter beats; 0: STILL */
static const char *const PULSE_LABEL[NPULSE] = {"STILL", "1/4", "1/2", "1", "2", "3", "4", "6", "8", "12", "16", "24", "32", "48", "64"};
#define NSHAPE 3u
static const char *const SHAPE_NAME[NSHAPE] = {"SINE", "MIX", "RANDOM"};
#define NSEASON 57u                             /* OFF, then 5 s .. 2 min in 5 s steps, then 15 s steps to 10 min */
#define SEASON_S(i) ((uint32_t)(i) <= 24u ? (uint32_t)(i) * 5u : 120u + ((uint32_t)(i) - 24u) * 15u)
#define NRATE 6u
static const uint8_t RATE_N[NRATE] = {1, 2, 3, 4, 6, 8};   /* cycle steps a beat */
static const char *const RATE_LABEL[NRATE] = {"1/4", "1/8", "1/8T", "1/16", "16T", "1/32"};

static int lfo_advance(lfo_t *l, uint32_t dt, uint32_t ms)   /* 1: a new random segment began (pick the next target) */
{
    uint32_t old = l->ph;
    if (!ms) return 0;
    l->ph += (4294967295u / ms) * dt;
    return (old >> (32u - l->seg)) != (l->ph >> (32u - l->seg));
}
static void lfo_target(lfo_t *l) { l->a = l->b; l->b = (int32_t)rnd(65535u) - 32767; }
static int32_t lfo_random(const lfo_t *l)       /* -32767..32767: from a to b along a raised cosine */
{
    uint32_t u = (l->ph << l->seg) >> 1;        /* where we are in this segment, 0..2^31 */
    int32_t w = (32767 - sine(u + 0x40000000u)) >> 1;   /* 0 .. 32767 */
    return l->a + ((((l->b - l->a) >> 1) * w) >> 14);
}
static void lfo_reset(void)                     /* PLAY: the waves start from the middle */
{
    pulse.ph = season.ph = roots.ph = 0;
    pulse.a = pulse.b = season.a = season.b = roots.a = roots.b = 0;
    pulse.armed = season.zone = roots.zone = 0;
    roots_dir = rnd(2u) ? 1 : -1;
    lfo_target(&pulse);
    season_dir = rnd(2u) ? 1 : -1;
    lfo_t0 = fm1_ms;
}
static int lfo_turn(lfo_t *l, uint32_t dt, uint32_t sea)   /* a season's slow random wave: 1 when it turns */
{
    int32_t v;
    if (lfo_advance(l, dt, SEASON_S(sea) * 700u)) lfo_target(l);
    if (!sea) return 0;
    v = lfo_random(l);                          /* a turn: the wave swings well up or well down (back near the middle */
    if (l->zone != 1u && v > 16384) { l->zone = 1; return 1; }   /*  before the same side turns it again) */
    if (l->zone != 2u && v < -16384) { l->zone = 2; return 1; }
    if ((l->zone == 1u && v < 8192) || (l->zone == 2u && v > -8192)) l->zone = 0;
    return 0;
}
static int lfo_tick(uint32_t bpm, uint32_t pul, uint32_t shp, uint32_t sea, uint32_t roo)   /* 1: the season turned, 2: ROOTS */
{
    uint32_t dt = fm1_ms - lfo_t0;
    int32_t v;
    int moved = 0;
    lfo_t0 = fm1_ms;
    if (!run || !dt) return 0;
    if (dt > 500u) dt = 500u;                   /* (after a stall: no leap) */
    if (lfo_advance(&pulse, dt, PULSE_Q[pul] * 15000u / bpm)) lfo_target(&pulse);
    if (PULSE_Q[pul]) {
        v = shp == 0u ? sine(pulse.ph) : shp == 2u ? lfo_random(&pulse) : (sine(pulse.ph) + lfo_random(&pulse)) >> 1;
        if (v < -4000) pulse.armed = 1;         /* (below the middle first: one move per rise) */
        else if (v > 0 && pulse.armed) { pulse.armed = 0; pulse_move(); }
    }
    if (lfo_turn(&season, dt, sea)) { season_turn(); moved |= 1; }
    if (lfo_turn(&roots, dt, roo)) { roots_turn(); moved |= 2; }
    return moved;
}

/* ---- the objects, the kit and the settings: one byte each, kept in flash.
 * An object: what it is, its four HOME knobs (0..20), its envelope, its cycle, and a seed: the things REC rolls that
 * no knob reaches (the overtones' ratios, the noise's colour, the picture's shape). The kit has NKIT places; the place
 * after them is the hand: the object just found, not yet kept. */
#define NKIT 8u
enum { G_ROCK, G_STICK, G_LEAF, G_PLANT, NGRP };
typedef struct { uint8_t on, grp, siz, mat, hol, stk, att, hld, dec, lvl, stp, hit, rot, rat, chc, wan, seed[4], lft, lfr, lfd, lfs; } obj_t;
#define NCHG 11u
static const uint8_t CHG_N[NCHG] = {0, 1, 2, 3, 4, 5, 6, 8, 10, 12, 16};   /* CHORDS: pulses a chord; 0: HOLD */
#define NATT 11u
static const uint8_t ATT_MS[NATT] = {0, 1, 2, 4, 7, 10, 15, 25, 40, 60, 100};
#define NHLD 11u
static const uint16_t HLD_MS[NHLD] = {0, 5, 10, 20, 30, 50, 75, 100, 150, 200, 300};
#define DEC0 15u                                /* DECAY 0 is DK[15]: 21 ms; its top DK[59]: 6 s */
#define NDEC 45u
static struct {
    uint8_t oct, ch, bpm, spl, tnc, scl, pul, shp, sea, roo, chg, cut, res, spc, mix, grp, cur;
    obj_t kit[NKIT + 1u];
} set;
enum { F_OCT, F_CH, F_BPM, F_SPL, F_TNC, F_SCL, F_PUL, F_SHP, F_SEA, F_ROO, F_CHG, F_CUT, F_RES, F_SPC, F_MIX, F_GRP, F_CUR,
       F_G,                                     /* (the settings end; from here the object's own) */
       F_SIZ = F_G, F_MAT, F_HOL, F_STK, F_ATT, F_HLD, F_DEC, F_LVL, F_STP, F_HIT, F_ROT, F_RAT, F_CHC, F_WAN,
       F_LFT, F_LFR, F_LFD, F_LFS,                 /* (the object's LFO: after its seed) */
       F_N, F_CPU = F_N, F_NONE };
/* an object's LFO: one of its settings moves by itself, so no two hits (or bars) are alike. MOVES: which; EVERY: the
 * wave's length in beats; DEPTH: how far (at the top, half the setting's range either way); WAVE: SINE, DRIFT (a
 * smooth random wander) or DICE (a new throw each hit; for the cycle's settings, each time round) */
#define NLFT 13u
#define NLFS 3u
static const char *const LFS_NAME[NLFS] = {"SINE", "DRIFT", "DICE"};
static const uint8_t CFG_MIN[F_N] = {0, 0, 40, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 1, 0, 0};
static const uint8_t CFG_MAX[F_N] = {255, 16, 200, NSPELL - 1u, 11, 11, NPULSE - 1u, NSHAPE - 1u, NSEASON - 1u, NSEASON - 1u,
    NCHG - 1u, NCUT - 1u, NRES - 1u, NSPACE - 1u, NMIX - 1u, NGRP - 1u, NKIT,
    20, 20, 20, 20, NATT - 1u, NHLD - 1u, NDEC - 1u, 20, 32, 32, 31, NRATE - 1u, 20, 20,
    NLFT - 1u, NPULSE - 1u, 20, NLFS - 1u};
static const uint8_t SET_DEF[F_G] = {0, 0, 100, 0, 0, 0, 8, 1, 0, 0, 5, NCUT - 1u, 0, 4, 3, G_ROCK, NKIT};
static const char *const F_NAME[F_N + 2] = {"OCT", "CH", "TEMPO", "SPELL", "TONIC", "SCALE", "PULSE", "SHAPE", "SEASON", "ROOTS",
    "CHORDS", "FILTER", "RES", "SPACE", "MIX", "FIND", "OBJECT",
    "SIZE", "MATTER", "HOLLOW", "STRIKE", "ATTACK", "HOLD", "DECAY", "LEVEL", "STEPS", "HITS", "ROTATE", "RATE", "CHANCE",
    "WANDER", "MOVES", "EVERY", "DEPTH", "WAVE", "CPU", ""};
static const char *const GRP_NAME[NGRP] = {"ROCK", "STICK", "LEAVES", "PLANT"};
static const char *const GRP_FIND[NGRP] = {"ROCKS", "STICKS", "LEAVES", "PLANTS"};
static const char *const PLANT_NAME[4] = {"STALK", "SAP", "BEND", "QUIVER"};   /* a plant's four HOME knobs: its own */
#define HAND (&set.kit[NKIT])
#define CUR (&set.kit[set.cur])
static uint8_t *obj_fp(const obj_t *o, uint32_t f)   /* an object's setting: its knobs in a row, then (past the seed) its LFO */
{
    return (uint8_t *)o + (f < F_LFT ? 2u + (f - F_SIZ) : 20u + (f - F_LFT));
}
static const uint8_t LFO_F[NLFT] = {F_NONE, F_SIZ, F_MAT, F_HOL, F_STK, F_ATT, F_HLD, F_DEC, F_LVL, F_HIT, F_ROT, F_CHC, F_WAN};   /* MOVES */
static uint8_t *fp(uint32_t f)                  /* where a setting lives: the settings, or the object in hand */
{
    return f < F_G ? (uint8_t *)&set + f : obj_fp(CUR, f);
}
static uint32_t cfg_max(uint32_t f)             /* the top of a setting, some of them by another */
{
    if (f == F_SCL) return SPELL[set.spl].n - 1u;
    if (f == F_HIT) return CUR->stp;
    if (f == F_ROT) return CUR->stp - 1u;
    return CFG_MAX[f];
}
/* the pages: HOME (the object's sound); ENV; ARP 1 (the cycle), 2 (chance, wander); FX; GLO (the tempo and the
 * harmony's pace); SEL (season, roots, shape) */
enum { P_HOME, P_ENV, P_ARP1, P_ARP2, P_FX, P_GLO, P_SEL, P_LFO, NPAGE };
static const char *const PAGE_NAME[NPAGE] = {"", "ENV", "ARP", "ARP 2", "FX", "GLO", "SEL", "LFO"};
static uint32_t page_field(uint32_t page, uint32_t k)
{
    static const uint8_t PF[NPAGE][4] = {{F_SIZ, F_MAT, F_HOL, F_STK}, {F_ATT, F_HLD, F_DEC, F_LVL}, {F_STP, F_HIT, F_ROT, F_RAT},
        {F_CHC, F_WAN, F_NONE, F_NONE}, {F_CUT, F_RES, F_SPC, F_MIX}, {F_BPM, F_PUL, F_CHG, F_TNC}, {F_SEA, F_ROO, F_SHP, F_SCL},
        {F_LFT, F_LFR, F_LFD, F_LFS}};
    uint32_t f = PF[page][k];
    return f >= F_G && f < F_N && !CUR->on ? F_NONE : f;   /* (nothing in hand: nothing to turn) */
}
static const char *fname(uint32_t f)            /* a setting's name: a plant's four HOME knobs have their own */
{
    return f >= F_SIZ && f <= F_STK && CUR->grp == G_PLANT ? PLANT_NAME[f - F_SIZ] : F_NAME[f];
}
static void apply(void)                         /* the settings to the sound */
{
    fx_cut = set.cut; fx_res = set.res; fx_space = set.spc; fx_mix = set.mix;
    chord_every = CHG_N[set.chg];
}

/* ---- finding an object (REC): everything about it at random, each in a range that stays a drum */
static uint32_t hsh(uint32_t x) { x *= 0x9E3779B1u; x ^= x >> 15; x *= 0x85EBCA6Bu; x ^= x >> 13; return x; }
static uint32_t obj_seed(const obj_t *o) { return o->seed[0] | (uint32_t)o->seed[1] << 8 | (uint32_t)o->seed[2] << 16 | (uint32_t)o->seed[3] << 24; }
static uint32_t dna(const obj_t *o, uint32_t i) { return (hsh(obj_seed(o) + i * 40503u) >> 9) & 255u; }   /* 0..255, the i-th */
static void find(obj_t *o, uint32_t grp)
{
    static const uint8_t LEAF_STP[4] = {8, 16, 12, 6};
    uint32_t s = grow_rng ^ (fm1_ms * 2654435761u), i;
    for (i = 0; i < 4u; i++) o->seed[i] = (uint8_t)(s >> (8u * i));
    o->on = 1; o->grp = (uint8_t)grp;
    o->siz = (uint8_t)(4u + rnd(13u)); o->mat = (uint8_t)(2u + rnd(17u)); o->hol = (uint8_t)rnd(21u); o->stk = (uint8_t)(3u + rnd(16u));
    o->att = 0; o->hld = 0; o->lvl = 16; o->rat = 3;   /* (the cycle in sixteenths) */
    o->lft = 0; o->lfr = 6; o->lfd = 8; o->lfs = 2;    /* (its LFO off; ready as DICE) */
    if (grp == G_PLANT) {                       /* a plant: the whole range of every knob, so no two alike */
        o->siz = (uint8_t)rnd(21u); o->mat = (uint8_t)rnd(21u); o->hol = (uint8_t)rnd(21u); o->stk = rnd(5u) < 2u ? 0u : (uint8_t)rnd(21u);
        o->att = rnd(4u) ? 0u : (uint8_t)rnd(8u); o->hld = rnd(3u) ? 0u : (uint8_t)rnd(6u);
        o->dec = (uint8_t)(4u + rnd(28u));
        o->stp = (uint8_t)(4u + rnd(13u)); o->hit = (uint8_t)(1u + rnd(o->stp / 2u)); o->rot = (uint8_t)rnd(o->stp);
        o->chc = (uint8_t)(12u + rnd(9u)); o->wan = (uint8_t)rnd(21u);
    } else if (grp == G_ROCK) {
        o->dec = (uint8_t)(12u + rnd(14u));     /* ~100 .. 500 ms */
        o->stp = 16; o->hit = (uint8_t)(1u + rnd(4u)); o->rot = rnd(3u) ? 0u : (uint8_t)rnd(16u);
        o->chc = 20; o->wan = 0;
    } else if (grp == G_STICK) {
        o->dec = (uint8_t)(6u + rnd(14u));
        o->stp = (uint8_t)(5u + rnd(12u)); o->hit = (uint8_t)(1u + rnd(o->stp / 2u)); o->rot = (uint8_t)rnd(o->stp);
        o->chc = (uint8_t)(14u + rnd(7u)); o->wan = (uint8_t)rnd(12u);
    } else {
        o->dec = (uint8_t)(4u + rnd(18u));
        o->stp = LEAF_STP[rnd(4u)]; o->hit = (uint8_t)(o->stp / 2u + rnd(o->stp / 4u + 1u)); o->rot = (uint8_t)rnd(2u);
        o->chc = (uint8_t)(12u + rnd(9u)); o->wan = (uint8_t)rnd(6u);
    }
}

/* ---- a hit (main loop): the object's knobs and seed, a note and a velocity -> a voice's numbers.
 * SIZE: the pitch (up to an octave either way, in steps of the scale; big is low), a longer ring, a slower fall.
 * MATTER: how hard it is: the overtones' level and ring, the noise's brightness (chalk .. flint, rotten .. dry
 *   hardwood, damp leaves .. dry seeds).
 * HOLLOW: rocks: a dead thud .. a booming tone whose pitch falls; sticks: a solid block (clangy overtones, short) .. a
 *   pipe (harmonic overtones, ringing, breathy); leaves: a loose pile (broad noise) .. seeds in a pod (a tuned ring).
 * STRIKE: what hits it: soft and dull .. a hard bright click; leaves: one flick .. a long scattered rattle. */
static int in_scale(uint32_t n)
{
    uint32_t i;
    for (i = 0; i < scale_n; i++) if (scale_pc[i] == n % 12u) return 1;
    return 0;
}
static uint32_t degrees(uint32_t n, int32_t d)  /* note n, down to the scale, then d steps along it */
{
    n = n < 36u ? 36u : n > 108u ? 108u : n;
    while (!in_scale(n)) n--;
    for (; d > 0; d--) do n++; while (!in_scale(n));
    for (; d < 0; d++) do n--; while (!in_scale(n));
    return n;
}
static uint32_t dk_ms(uint32_t ms)              /* the decay nearest a time (-60 dB) */
{
    uint32_t i = 0;
    while (i + 1u < NDK && DK_MS[i] < ms) i++;
    return DK[i];
}
static uint32_t band_f(int32_t note)            /* a band-pass's centre from a note: 2 sin(pi f / FS), near enough, Q15 */
{
    uint32_t f = (uint32_t)(((uint64_t)note_inc((uint32_t)(note < 12 ? 12 : note > 124 ? 124 : note)) * 205887u) >> 32);
    return f < 200u ? 200u : f > 30000u ? 30000u : f;
}
static uint32_t tick_next, tick_n;              /* the cycles' clock: the next tick's time (ms x16); ticks since PLAY (24 a beat) */
static uint8_t dice_hit[NKIT + 1u], dice_cyc[NKIT + 1u];   /* each place's throw: new after every hit; new each time its cycle comes round */
static int32_t lfo_wave(uint32_t slot, int cyc) /* an object's LFO now: -256 .. 256 */
{
    const obj_t *o = &set.kit[slot];
    uint32_t per = PULSE_Q[o->lfr] * 6u, t = run ? tick_n : fm1_ms * set.bpm / 2500u, i = t / per, ph = t % per;   /* in ticks */
    int32_t a, b;
    if (o->lfs == 0u) return sine(ph * (0xFFFFFFFFu / per)) >> 7;
    if (o->lfs == 2u) return (int32_t)(cyc ? dice_cyc[slot] : dice_hit[slot]) * 2 - 255;
    a = (int32_t)(hsh(obj_seed(o) + i) & 511u) - 256; b = (int32_t)(hsh(obj_seed(o) + i + 1u) & 511u) - 256;
    return a + (b - a) * (int32_t)ph / (int32_t)per;
}
static uint32_t eff(uint32_t slot, uint32_t f)  /* an object's setting as it sounds now: its value, moved by its LFO if that is its target */
{
    const obj_t *o = &set.kit[slot];
    int32_t v = *obj_fp(o, f), lo = CFG_MIN[f], hi = f == F_HIT ? o->stp : f == F_ROT ? o->stp - 1 : CFG_MAX[f];
    if (!o->lft || LFO_F[o->lft] != f || !o->lfd || (f == F_HIT && !v)) return (uint32_t)v;   /* (silenced stays silent) */
    v += lfo_wave(slot, f >= F_STP) * (hi - lo) * (int32_t)o->lfd / 10240;
    return (uint32_t)(v < lo ? lo : v > hi ? hi : v);
}
static const int8_t GOFF[NGRP] = {-24, 12, 0, 0};  /* where each kind sits: rocks two octaves under the keys, sticks one over */
static const uint16_t VGAIN[NGRP] = {28000, 22000, 26000, 26000};   /* a hit's top level, Q15 */
static uint32_t ages, hit_ms[NKIT + 1u];        /* when each place last sounded (the screen) */
static void strike(uint32_t slot, uint32_t keynote, uint32_t vel)
{
    const obj_t *o = &set.kit[slot];
    int32_t S = (int32_t)eff(slot, F_SIZ), M = (int32_t)eff(slot, F_MAT), H = (int32_t)eff(slot, F_HOL), K = (int32_t)eff(slot, F_STK), note, att_ms, ix;
    uint32_t v, pick = 0, best = 0xFFFFFFFFu;
    vpar_t p = {0};
    if (!o->on || !vel) return;
    note = (int32_t)degrees(keynote, o->grp == G_PLANT ? 0 : (10 - S) * 7 / 10) + GOFF[o->grp];
    note = note < 12 ? 12 : note > 119 ? 119 : note;
    p.inc = note_inc((uint32_t)note);
    p.peak = (int32_t)((uint32_t)VGAIN[o->grp] * eff(slot, F_LVL) / 20u * (60u + vel) / 187u);
    att_ms = ATT_MS[eff(slot, F_ATT)];
    ix = (int32_t)(DEC0 + eff(slot, F_DEC)) + (o->grp == G_PLANT ? 0 : (S - 10) / 4);
    p.mdk = DK[ix < 0 ? 0 : ix > (int32_t)NDK - 1 ? (int32_t)NDK - 1 : ix];
    p.gpk = 65536u;
    switch (o->grp) {
    case G_ROCK:
        p.r2 = 5734u + dna(o, 1) * 16u;         /* 1.4 .. 2.4: a stone's overtones are nowhere near harmonic */
        p.r3 = 10650u + dna(o, 2) * 35u;        /* 2.6 .. 4.8 */
        p.lv[0] = 26000;
        p.lv[1] = M * 900 + (int32_t)dna(o, 3) * 10;
        p.lv[2] = M * 600 + (int32_t)dna(o, 4) * 8;
        p.dk[0] = H >= 14 ? 65536u : dk_ms((uint32_t)(70 + H * H * 4));
        p.dk[1] = dk_ms((uint32_t)(30 + M * 14 + S * 2));
        p.dk[2] = dk_ms((uint32_t)(18 + M * 9 + S));
        p.drop = 600 + H * 420 + (int32_t)dna(o, 0) * 6;
        p.drop_k = dk_ms((uint32_t)(40 + H * 8 + S * 4));
        p.nf = (int32_t)band_f(note + 34 + K + M / 2 + (int32_t)(dna(o, 5) >> 5));
        p.nq = 30000; p.nhp = K * 6;
        p.nlv = 2500 + K * 1300;
        p.ndk = dk_ms((uint32_t)(10 + (20 - K) * 2 + S));
        att_ms += (20 - K) / 5;
        break;
    case G_STICK: {
        int32_t b2 = 9830 + (int32_t)dna(o, 1) * 14, b3 = 19660 + (int32_t)dna(o, 2) * 29;   /* a bar: 2.4 .. 3.3, 4.8 .. 6.6 */
        int32_t open = (int32_t)(dna(o, 6) & 1u), p2 = open ? 8192 : 12288, p3 = open ? 12288 : 20480;   /* a pipe: 2, 3 or 3, 5 */
        p.r2 = (uint32_t)(b2 + (p2 - b2) * H / 20);
        p.r3 = (uint32_t)(b3 + (p3 - b3) * H / 20);
        p.lv[0] = 24000;
        p.lv[1] = 5000 + M * 800 + (int32_t)dna(o, 3) * 10;
        p.lv[2] = 1500 + M * 650 + (int32_t)dna(o, 4) * 6;
        p.dk[0] = H >= 12 ? 65536u : dk_ms((uint32_t)(60 + H * H * 5));
        p.dk[1] = dk_ms((uint32_t)(25 + M * 7 + H * 10));
        p.dk[2] = dk_ms((uint32_t)(15 + M * 4 + H * 6));
        p.drop = (20 - M) * 22 + (int32_t)dna(o, 0);
        p.drop_k = dk_ms(25u);
        p.nf = (int32_t)band_f(note + 26 + K - H / 2);
        p.nq = 30000 - H * 1100; p.nhp = K * 4;
        p.nlv = 2500 + K * 1000 + H * 300;
        p.ndk = dk_ms((uint32_t)(8 + (20 - K) + H * 6));
        att_ms += (20 - K) / 7;
        break;
    }
    case G_PLANT: {                             /* FM: a stem that rings, croaks, buzzes or springs (the knobs: STALK, SAP, BEND, QUIVER) */
        static const uint16_t STALK[21] = {     /* the modulator's ratio, Q12: whole numbers (reedy, hollow), halves, then nothing harmonic (bells, thorns) */
            4096, 8192, 12288, 16384, 20480, 28672, 2048, 6144, 10240, 14336,
            5776, 7086, 9175, 11305, 12861, 14787, 17162, 22241, 27812, 33628, 45466};
        static const uint16_t SECOND[4] = {4110, 8192, 2048, 6144};   /* the second carrier: a hair off, an octave up or down, a fifth */
        int32_t amt = H < 10 ? 10 - H : H - 10;
        uint32_t f = 768u, i;                   /* the quiver: 3 Hz, a quarter faster a step, to ~200 Hz (a buzz) */
        p.fm = 1;
        p.r2 = STALK[S];
        p.r3 = SECOND[dna(o, 6) & 3u];
        p.lv[0] = 22000;
        p.lv[1] = M * 1600;                     /* SAP: how far the phase bends (dry: a pure tone; full of sap: bright, harsh) */
        p.lv[2] = (int32_t)dna(o, 4) * 40;
        p.dk[0] = 65536u;
        p.dk[1] = M >= 18 ? 65536u : dk_ms((uint32_t)(40 + (int32_t)dna(o, 3) * 2 + M * M * 3));   /* ... and how long it stays bent */
        p.dk[2] = dk_ms(100u + dna(o, 2) * 4u);
        p.drop = amt * amt * 30;                /* BEND: under 10 the note falls into place, over 10 it springs up to it */
        p.rise = H > 10;
        p.drop_k = dk_ms((uint32_t)(30 + (int32_t)dna(o, 0) + amt * 12));
        for (i = 1; i < (uint32_t)K; i++) f = f * 5u / 4u;
        p.trinc = f * 6087u;
        p.trdep = K ? (uint32_t)(22000 + K * 2100) : 0u;
        p.nf = (int32_t)band_f(note + 24 + (int32_t)(dna(o, 5) >> 4));
        p.nq = 24000;
        p.nlv = 1500 + (int32_t)dna(o, 1) * 12;   /* (a small pluck) */
        p.ndk = dk_ms(8u + (dna(o, 7) >> 4));
        break;
    }
    default:
        p.r2 = p.r3 = 4096u;
        p.dk[0] = p.dk[1] = p.dk[2] = 65536u;
        p.drop_k = 65536u;
        p.nf = (int32_t)band_f(note + 34 + M / 2 + (int32_t)(dna(o, 1) >> 5));
        p.nq = 30000 - H * 1350;                /* a pod: Q up to ~11, the band rings on its note */
        p.nhp = M * 9;
        p.nlv = 14000 + (p.nq >> 1);            /* (a narrow band lets less noise through: evened out below) */
        p.nlv = p.nlv * (40 + H) / 40;
        if (p.nlv > 32767) p.nlv = 32767;
        p.ndk = 65536u;
        p.gp = (uint32_t)((2200 - K * 95) * (12 + S) / 22);
        p.gpk = dk_ms((uint32_t)(10 + K * K * 2));
        p.gfall = 190 + M * 65;
        break;
    }
    p.att = (uint32_t)att_ms * 2756u / 1000u;
    p.hold = (uint32_t)HLD_MS[eff(slot, F_HLD)] * 2756u / 1000u;
    for (v = 0; v < NV; v++) {                  /* a silent voice, else the oldest */
        uint32_t score = voice[v].stage || voice[v].trig != voice[v].tseen ? 0x80000000u + voice[v].age : 0u;   /* (a hit the audio has not taken yet counts: two hits at once get a voice each) */
        if (score < best) { best = score; pick = v; }
    }
    voice[pick].nx = p;
    voice[pick].slot = (uint8_t)slot;
    voice[pick].age = ++ages;
    voice[pick].trig++;                         /* (last: the audio takes the numbers with it) */
    hit_ms[slot] = fm1_ms ? fm1_ms : 1u;
    dice_hit[slot] = (uint8_t)rnd(256u);        /* (the next hit's throw) */
#ifdef DINGHY_HOST
    printf("%7u ms  %s %u: note %d, vel %u, voice %u\n", (unsigned)fm1_ms, GRP_NAME[o->grp], (unsigned)slot, (int)note, (unsigned)vel, (unsigned)pick);
#endif
}
static int quiet(void)                          /* nothing sounds, not even a tail */
{
    uint32_t v;
    if (run) return 0;
    for (v = 0; v < NV; v++) if (voice[v].stage || voice[v].trig != voice[v].tseen) return 0;
    return 1;
}

/* ---- the cycles (main loop): every object its own Euclidean rhythm: STEPS steps of RATE, HITS of them spread as
 * evenly as they go, turned by ROTATE; the first step is the accent; CHANCE: how likely a hit sounds. All run on one
 * clock of 24 ticks a beat, so they stay in step whatever their rates and lengths, and an object kept while the
 * cycles run falls straight in. A hit plays the chord's root nearest the middle of the keys; WANDER lets it stray to
 * the chord's other notes, and now and then an octave away. */
static int euclid(uint32_t i, uint32_t steps, uint32_t hits, uint32_t rot)   /* step i is a hit */
{
    if (!hits) return 0;
    if (hits >= steps) return 1;
    return ((i + steps - rot % steps) % steps) * hits % steps < hits;
}
static uint32_t cyc_step(const obj_t *o) { return tick_n / (24u / RATE_N[o->rat]) % o->stp; }   /* the step it is on */
static void cycle_tick(void)
{
    uint32_t now = fm1_ms << 4, len = 40000u / set.bpm, s;   /* a tick, in ms x16 */
    if (!run) { tick_next = now; tick_n = 0; return; }
    if ((int32_t)(now - tick_next) > 16000) tick_next = now;   /* (after a stall: back on time, no burst) */
    while ((int32_t)(now - tick_next) >= 0) {
        for (s = 0; s <= NKIT; s++) {
            const obj_t *o = &set.kit[s];
            uint32_t div = 24u / RATE_N[o->rat], st, k = 0, n, wan;
            if (!o->on || tick_n % div) continue;
            st = tick_n / div % o->stp;
            if (st == 0u) dice_cyc[s] = (uint8_t)rnd(256u);   /* (round again: a new throw for the cycle's settings) */
            if (!euclid(st, o->stp, eff(s, F_HIT), eff(s, F_ROT)) || rnd(20u) >= eff(s, F_CHC)) continue;
            wan = eff(s, F_WAN);
            if (rnd(20u) < wan) k = rnd(4u);
            n = (uint32_t)((int32_t)65 + 12 * (int8_t)set.oct);   /* F4, the middle of the keys: the nearest below or just above */
            while (n % 12u != cpc(chord_now, k)) n--;
            if (wan > 10u && !rnd(6u)) n += rnd(2u) ? 12u : (uint32_t)-12;
            strike(s, n, st == 0u ? 127u : 70u + rnd(40u));
        }
        tick_n++;
        tick_next += len;
    }
}

/* ---- the screen. HOME: the object in hand as a picture (its knobs change it: SIZE its size, MATTER its surface,
 * HOLLOW its hole, STRIKE its marks) and the kit as a row of eight; words only for a moment. The other pages: the
 * picture (ARP: the cycle as a ring; GLO, SEL: the spell and its scale) over the page's four knobs. */
#define C_ACC RGB(200, 156, 88)                 /* bark */
#define C_OFF RGB(90, 90, 90)
#define KEY0 53u                                /* key 0 = F3 */
#define KEY_MUTE 25u                            /* F#5: the object in hand falls silent (its hits to 0); again: back */
static const uint8_t WHITE[16] = {0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23, 24, 26};   /* the white keys, left to right */
static const uint8_t GRP_RGB[NGRP][3] = {{176, 176, 164}, {200, 156, 88}, {120, 170, 90}, {214, 140, 170}};   /* stone, bark, leaf, blossom */
static uint16_t tone(uint32_t grp, int32_t l)   /* a kind's colour at light l: 0 dark .. 160 itself .. 255 pale */
{
    int32_t r = GRP_RGB[grp][0], g = GRP_RGB[grp][1], b = GRP_RGB[grp][2];
    if (l <= 0) return C_BLACK;
    if (l > 255) l = 255;
    if (l < 160) return RGB(r * l / 160, g * l / 160, b * l / 160);
    l -= 160;
    return RGB(r + (255 - r) * l / 95, g + (255 - g) * l / 95, b + (255 - b) * l / 95);
}
static void put(char **p, const char *a) { while (*a) *(*p)++ = *a++; }
static void put_num(char **p, uint32_t n) { if (n >= 1000u) *(*p)++ = (char)('0' + n / 1000u % 10u); if (n >= 100u) *(*p)++ = (char)('0' + n / 100u % 10u); if (n >= 10u) *(*p)++ = (char)('0' + n / 10u % 10u); *(*p)++ = (char)('0' + n % 10u); }
static void put_ms(char **p, uint32_t ms)       /* 45ms, 1.2s */
{
    if (ms < 1000u) { put_num(p, ms); put(p, "ms"); return; }
    put_num(p, ms / 1000u); *(*p)++ = '.'; put_num(p, ms / 100u % 10u); *(*p)++ = 's';
}
static int32_t sx(uint32_t a, int32_t r) { return (sine(a + 0x40000000u) * r) >> 15; }   /* r cos a (a: a full turn is 2^32) */
static int32_t sy(uint32_t a, int32_t r) { return (sine(a) * r) >> 15; }                 /* r sin a */
#define TURN(i, n) (0xFFFFFFFFu / (n) * (i))    /* the i-th of n round a circle */
static void cv_ell(int32_t cx, int32_t cy, int32_t rx, int32_t ry, uint16_t c, uint32_t dots)   /* an ellipse; dots: every other piece */
{
    uint32_t i;
    for (i = 0; i < 32u; i++)
        if (!dots || !(i & 1u))
            cv_line(cx + sx(TURN(i, 32u), rx), cy + sy(TURN(i, 32u), ry), cx + sx(TURN(i + 1u, 32u), rx), cy + sy(TURN(i + 1u, 32u), ry), c);
}

#define OB_Y 24u                                /* the picture: 240 x 120 at (0, 24) */
#define OB_H 120
static void draw_rock(const obj_t *o, int32_t l)
{
    int32_t S = o->siz, M = o->mat, H = o->hol, K = o->stk, rx = 26 + S * 13 / 5, ry = rx * 5 / 8, cx = 120, cy = 62, x[12], y[12];
    uint32_t i;
    uint16_t c = tone(G_ROCK, l), dim = tone(G_ROCK, l * 3 / 8);
    for (i = 0; i < 12u; i++) {                 /* its outline: twelve corners, the harder the more jagged */
        int32_t k = 256 - (int32_t)(dna(o, 8u + i) % (uint32_t)(24 + M * 5));
        x[i] = cx + sx(TURN(i, 12u), rx * k >> 8); y[i] = cy + sy(TURN(i, 12u), ry * k >> 8);
    }
    for (i = 0; i < 12u; i++) cv_line(x[i], y[i], x[(i + 1u) % 12u], y[(i + 1u) % 12u], c);
    for (i = 0; i < (uint32_t)(M / 4); i++) {   /* facets: flint breaks in planes */
        uint32_t a = dna(o, 20u + i) % 12u;
        cv_line(x[a], y[a], cx + ((int32_t)dna(o, 26u + i) - 128) * rx / 400, cy + ((int32_t)dna(o, 32u + i) - 128) * ry / 400, dim);
    }
    for (i = 0; i < (uint32_t)(20 - M) * 2u; i++)   /* chalk: a soft, speckled face */
        cv_pset(cx + ((int32_t)dna(o, 40u + i) - 128) * rx / 210, cy + ((int32_t)dna(o, 90u + i) - 128) * ry / 210, dim);
    if (H > 2) {                                /* its hollow */
        cv_ell(cx, cy, rx * H / 44, ry * H / 44, c, 0);
        if (H > 11) cv_ell(cx + 1, cy + 1, rx * (H - 8) / 44, ry * (H - 8) / 44, dim, 0);
    }
    for (i = 0; i < (uint32_t)(K / 4); i++) {   /* where it is struck: the harder, the more chips fly */
        uint32_t a = 0xA0000000u + (uint32_t)(i * 0x09000000u);
        cv_line(cx + sx(a, rx + 6), cy + sy(a, ry + 6), cx + sx(a, rx + 10 + K / 3), cy + sy(a, ry + 10 + K / 3), dim);
    }
}
static int32_t stick_y(const obj_t *o, int32_t u, int32_t len)   /* its middle line at u of 32 along: a slope and a bend */
{
    return 62 - (u - 16) * len / 160 + ((int32_t)dna(o, 7) - 128) * u * (32 - u) / 4096;
}
static void draw_stick(const obj_t *o, int32_t l)
{
    int32_t S = o->siz, M = o->mat, H = o->hol, K = o->stk, len = 30 + S * 7 / 2, t = 3 + S / 3, u, x0, x1, y0, y1;
    uint32_t i;
    uint16_t c = tone(G_STICK, l), dim = tone(G_STICK, l * 3 / 8);
    for (u = 0; u < 32; u++) {
        x0 = 120 - len + u * len / 16; x1 = 120 - len + (u + 1) * len / 16;
        y0 = stick_y(o, u, len); y1 = stick_y(o, u + 1, len);
        if (M >= 7 || dna(o, 50u + (uint32_t)u) % 8u >= (uint32_t)(7 - M) / 2u) cv_line(x0, y0 - t, x1, y1 - t, c);   /* rotten: its bark in gaps */
        if (M >= 7 || dna(o, 90u + (uint32_t)u) % 8u >= (uint32_t)(7 - M) / 2u) cv_line(x0, y0 + t, x1, y1 + t, c);
        if (M > 11 && (u & 1)) cv_line(x0, y0 - t / 3, x1, y1 - t / 3, dim);   /* hardwood: its grain */
        if (M > 16 && !(u & 1)) cv_line(x0, y0 + t / 3, x1, y1 + t / 3, dim);
        if (H > 9 && (u & 3) == 1) cv_line(x0, y0, x1, y1, dim);   /* a pipe: the bore, dashed */
    }
    y0 = stick_y(o, 0, len); y1 = stick_y(o, 32, len);
    cv_line(120 - len, y0 - t, 120 - len, y0 + t, c);
    if (H > 4) cv_ell(120 + len, y1, 1 + H / 5, t, c, 0);   /* the open end */
    else cv_line(120 + len, y1 - t, 120 + len, y1 + t, c);
    for (i = 0; i < 1u + dna(o, 3) % 3u; i++) { /* knots */
        u = 4 + (int32_t)(dna(o, 10u + i) % 24u);
        cv_ell(120 - len + u * len / 16, stick_y(o, u, len), 2, t / 2 > 1 ? t / 2 : 1, dim, 0);
    }
    for (i = 0; M < 8 && i < (uint32_t)(8 - M) * 2u; i++) {   /* rotten: fungus along its underside */
        u = (int32_t)(dna(o, 20u + i) % 32u);
        cv_rect(120 - len + u * len / 16, stick_y(o, u, len) + t + 2 + (int32_t)(dna(o, 40u + i) % 4u), 2, 2, dim);
    }
    for (i = 0; i < (uint32_t)(K / 5); i++)     /* where it is struck */
        cv_line(120 - 6 + (int32_t)i * 6, stick_y(o, 16, len) - t - 4, 120 - 9 + (int32_t)i * 6, stick_y(o, 16, len) - t - 8 - K / 4, dim);
}
static void draw_leaves(const obj_t *o, int32_t l)
{
    int32_t S = o->siz, M = o->mat, H = o->hol, K = o->stk, n = 6 + S * 2, wide = 28 + S * 2 + K, high = 18 + S * 2, i;
    uint16_t leaf = tone(G_LEAF, l), seed = tone(G_STICK, l), dim = tone(G_LEAF, l * 3 / 8);
    for (i = 0; i < n; i++) {                   /* a heap: leaves and, the drier the more, seeds */
        int32_t dx = ((int32_t)dna(o, 10u + (uint32_t)i) - 128) * wide / 128, top = high * (wide * wide - dx * dx) / (wide * wide);
        int32_t x = 120 + dx, y = 96 - (int32_t)(dna(o, 70u + (uint32_t)i) % (uint32_t)(top + 1)), r = 3 + (int32_t)(dna(o, 130u + (uint32_t)i) % 4u);
        if ((int32_t)(dna(o, 190u + (uint32_t)i) % 20u) < M) cv_rect(x - 1, y - 1, 3, 3, seed);
        else {
            int32_t tx = (dna(o, 250u + (uint32_t)i) & 1u) ? r : -r;
            cv_line(x - r, y, x + tx / 2, y - r / 2 - 1, leaf); cv_line(x + tx / 2, y - r / 2 - 1, x + r, y, leaf);
            cv_line(x + r, y, x - tx / 2, y + r / 2 + 1, leaf); cv_line(x - tx / 2, y + r / 2 + 1, x - r, y, leaf);
        }
    }
    for (i = 0; i < K; i++)                     /* a rattle: what flies up */
        cv_pset(120 + ((int32_t)dna(o, 300u + (uint32_t)i) - 128) * (wide + 14) / 128, 92 - high - (int32_t)(dna(o, 330u + (uint32_t)i) % (uint32_t)(6 + K)), dim);
    if (H > 5) cv_ell(120, 98 - high / 2, wide + 10, high / 2 + 14, H > 13 ? leaf : dim, H < 14);   /* a pod round them */
    else cv_line(120 - wide - 8, 99, 120 + wide + 8, 99, dim);   /* (the ground) */
}
static void draw_plant(const obj_t *o, int32_t l)   /* STALK: how thick, and thorns; SAP: its leaves; BEND: which way it leans; QUIVER: its tremble */
{
    int32_t S = o->siz, M = o->mat, H = o->hol, K = o->stk, t, x0 = 120, y0 = 110, x = 120, y = 110, w = 1 + S / 6, j, r = 3 + (int32_t)(dna(o, 9) % 5u);
    uint16_t c = tone(G_PLANT, l), leaf = tone(G_LEAF, l), dim = tone(G_PLANT, l * 3 / 8);
    cv_line(70, 111, 170, 111, tone(G_LEAF, l * 3 / 8));   /* (the ground) */
    for (t = 1; t <= 24; t++) {
        x = 120 + (H - 10) * t * t / 96 + sy((uint32_t)t * 0x15555555u, K * (t > 4 ? 1 : 0) / 3);
        y = 110 - t * (74 + (int32_t)(dna(o, 8) % 16u)) / 24;
        for (j = 0; j < w; j++) cv_line(x0 + j - w / 2, y0, x + j - w / 2, y, c);
        if (M > 1 && t % (M > 12 ? 2 : M > 6 ? 3 : 5) == 0 && t < 23) {   /* a leaf, left and right in turn */
            int32_t dir = (t / (M > 12 ? 2 : M > 6 ? 3 : 5)) & 1 ? 1 : -1, len = 5 + M / 2;
            cv_line(x, y, x + dir * len, y - len / 3, leaf); cv_line(x + dir * len, y - len / 3, x + dir * len / 2, y - len * 2 / 3, leaf);
            cv_line(x + dir * len / 2, y - len * 2 / 3, x, y, leaf);
        }
        if (S > 13 && (t & 3) == 2) cv_line(x + w / 2, y, x + w / 2 + 3, y - 2, dim);   /* thorns */
        x0 = x; y0 = y;
    }
    cv_ell(x, y - r, r, r, c, 0);               /* its head */
    if (dna(o, 6) & 1u) cv_ell(x, y - r, r + 3, r + 3, dim, 1);
    for (j = 0; j < K / 3; j++) {               /* a quiver: it trembles */
        cv_line(x + r + 6 + j * 4, y - r - 4, x + r + 8 + j * 4, y - r + 4, dim);
        cv_line(x - r - 6 - j * 4, y - r - 4, x - r - 8 - j * 4, y - r + 4, dim);
    }
}
static void draw_env(const obj_t *o)            /* ENV: the envelope as a line: up (ATTACK), along (HOLD), away (DECAY); LEVEL its height */
{
    static const uint8_t FALL[9] = {255, 166, 108, 70, 45, 29, 19, 12, 0};   /* a decay's shape, eighths of its length */
    int32_t wa = 3 + (int32_t)ATT_MS[o->att] * 2 / 5, wh = (int32_t)HLD_MS[o->hld] / 6, wd = 14 + (int32_t)o->dec * 3;
    int32_t h = 12 + (int32_t)o->lvl * 4, base = 104, x = (240 - (wa + wh + wd)) / 2, i, px, py;
    uint16_t c, dim;
    cv_begin(240, OB_H, C_BLACK);
    if (o->on) {
        c = tone(o->grp, 170); dim = tone(o->grp, 60);
        cv_line(8, base, 232, base, RGB(45, 45, 45));
        cv_line(x, base, x + wa, base - h, c);
        cv_line(x + wa, base - h, x + wa + wh, base - h, c);
        px = x + wa + wh; py = base - h;
        for (i = 1; i <= 8; i++) {
            int32_t nx = x + wa + wh + wd * i / 8, ny = base - h * FALL[i] / 255;
            cv_line(px, py, nx, ny, c);
            if (i < 8) cv_line(nx, ny + 2, nx, base, dim);   /* (shaded under the fall) */
            px = nx; py = ny;
        }
        cv_rect(x - 1, base - 1, 3, 3, c); cv_rect(x + wa - 1, base - h - 1, 3, 3, c);   /* the corners */
        cv_rect(x + wa + wh - 1, base - h - 1, 3, 3, c); cv_rect(x + wa + wh + wd - 1, base - 1, 3, 3, c);
    }
    cv_blit(0, OB_Y);
}
static void draw_find(void)                     /* HOME, top left: the four kinds; the one REC finds bright (PRESETS) */
{
    uint32_t g;
    cv_begin(68, 20, C_BLACK);
    for (g = 0; g < NGRP; g++) {
        int32_t x = (int32_t)g * 17;
        uint16_t c = tone(g, g == set.grp ? 220 : 50);
        if (g == G_ROCK) { cv_rect(x + 4, 3, 8, 10, c); cv_rect(x + 2, 5, 12, 6, c); }
        else if (g == G_STICK) { cv_line(x + 2, 12, x + 13, 4, c); cv_line(x + 2, 13, x + 13, 5, c); cv_line(x + 3, 14, x + 14, 6, c); }
        else if (g == G_LEAF) { cv_rect(x + 2, 9, 4, 4, c); cv_rect(x + 7, 4, 4, 4, c); cv_rect(x + 10, 10, 4, 4, c); }
        else { cv_rect(x + 7, 7, 2, 8, c); cv_rect(x + 5, 2, 6, 5, c); cv_rect(x + 9, 10, 4, 2, c); cv_rect(x + 3, 12, 4, 2, c); }
        if (g == set.grp) cv_rect(x + 2, 17, 12, 2, c);
    }
    cv_blit(4, 2);
}
static void draw_lfo(uint32_t slot)             /* LFO: its wave over two lengths, as deep as DEPTH; where it is now */
{
    const obj_t *o = &set.kit[slot];
    int32_t x, py = 0, h = 6 + (int32_t)o->lfd * 2, mid = 60;
    uint32_t per = PULSE_Q[o->lfr] * 6u, t = run ? tick_n : fm1_ms * set.bpm / 2500u;
    uint16_t c;
    cv_begin(240, OB_H, C_BLACK);
    if (o->on) {
        c = tone(o->grp, o->lft ? 170 : 55);
        cv_line(20, mid, 220, mid, RGB(45, 45, 45));
        for (x = 0; x <= 200; x++) {
            int32_t y;
            if (o->lfs == 0u) y = sine((uint32_t)x * (0xFFFFFFFFu / 100u)) >> 7;
            else if (o->lfs == 1u) {
                int32_t a = (int32_t)(hsh(obj_seed(o) + (uint32_t)(x / 50)) & 511u) - 256, b = (int32_t)(hsh(obj_seed(o) + (uint32_t)(x / 50) + 1u) & 511u) - 256;
                y = a + (b - a) * (x % 50) / 50;
            } else y = (int32_t)(hsh(obj_seed(o) + (uint32_t)(x / 20)) & 511u) - 256;
            y = mid - y * h / 256;
            if (x) cv_line(20 + x - 1, py, 20 + x, y, c);
            py = y;
        }
        if (o->lft && o->lfs != 2u) {           /* where it is now (DICE has no place: it is thrown) */
            x = 20 + (int32_t)(t % (per * 2u) * 200u / (per * 2u));
            cv_line(x, mid - h - 6, x, mid + h + 6, tone(o->grp, 255));
        }
    }
    cv_blit(0, OB_Y);
}
static void draw_object(const obj_t *o, int flare)
{
    cv_begin(240, OB_H, C_BLACK);
    if (!o->on) cv_text((240 - text_w(&FONT_S, "REC finds one")) / 2, 52, &FONT_S, "REC finds one", C_OFF);
    else if (o->grp == G_ROCK) draw_rock(o, flare ? 250 : 160);
    else if (o->grp == G_STICK) draw_stick(o, flare ? 250 : 160);
    else if (o->grp == G_PLANT) draw_plant(o, flare ? 250 : 160);
    else draw_leaves(o, flare ? 250 : 160);
    cv_blit(0, OB_Y);
}
#define KIT_Y 156u                              /* the kit: eight places, 30 x 28 each */
static void draw_kit(uint32_t now)
{
    uint32_t i;
    cv_begin(240, 28, C_BLACK);
    for (i = 0; i < NKIT; i++) {
        const obj_t *o = &set.kit[i];
        int32_t x = (int32_t)i * 30, l = hit_ms[i] && now - hit_ms[i] < 110u ? 255 : o->hit ? 150 : 60;   /* (no hits: dim) */
        if (!o->on) { cv_rect(x + 14, 11, 2, 2, RGB(60, 60, 60)); continue; }
        if (o->grp == G_ROCK) { cv_rect(x + 10, 6, 10, 12, tone(G_ROCK, l)); cv_rect(x + 8, 9, 14, 6, tone(G_ROCK, l)); }
        else if (o->grp == G_STICK) { cv_line(x + 7, 17, x + 23, 7, tone(G_STICK, l)); cv_line(x + 7, 18, x + 23, 8, tone(G_STICK, l)); cv_line(x + 8, 19, x + 24, 9, tone(G_STICK, l)); }
        else if (o->grp == G_PLANT) { cv_rect(x + 14, 9, 2, 11, tone(G_PLANT, l)); cv_rect(x + 12, 4, 6, 5, tone(G_PLANT, l)); cv_rect(x + 16, 13, 4, 2, tone(G_PLANT, l)); cv_rect(x + 10, 15, 4, 2, tone(G_PLANT, l)); }
        else { cv_rect(x + 9, 13, 4, 4, tone(G_LEAF, l)); cv_rect(x + 15, 8, 4, 4, tone(G_LEAF, l)); cv_rect(x + 18, 15, 4, 4, tone(G_LEAF, l)); }
        if (i == set.cur) cv_rect(x + 5, 24, 20, 2, C_ACC);
    }
    cv_blit(0, KIT_Y);
}
/* ARP: the cycle of the object in hand as a ring. Its steps round a circle from the top, the hits joined into the
 * cycle's figure; the step now runs round (the hand), a hit flares as it sounds; the first step (the accent) ringed. */
static void arp_pt(uint32_t i, uint32_t n, int32_t r, int32_t *x, int32_t *y)
{
    uint32_t a = TURN(i, n) - 0x40000000u;
    *x = 120 + sx(a, r); *y = OB_H / 2 + sy(a, r);
}
static void draw_arp(uint32_t now)
{
    const obj_t *o = CUR;
    uint32_t n = o->stp, h = eff(set.cur, F_HIT), ro = eff(set.cur, F_ROT), i, first = n, g = o->grp;   /* (as its LFO has them now) */
    int fl = run && hit_ms[set.cur] && now - hit_ms[set.cur] < 200u;
    int32_t x, y, x0 = 0, y0 = 0, fx = 0, fy = 0, cur = run ? (int32_t)cyc_step(o) : -1;
    cv_begin(240, OB_H, C_BLACK);
    if (o->on) {
        for (i = 0; i < 72u; i += 2u) {         /* the circle, dotted */
            arp_pt(i, 72u, 50, &x, &y); arp_pt(i + 1u, 72u, 50, &x0, &y0);
            cv_line(x, y, x0, y0, tone(g, 50));
        }
        for (i = 0; i < n; i++) {               /* the figure: hit to hit */
            if (!euclid(i, n, h, ro)) continue;
            arp_pt(i, n, 50, &x, &y);
            if (first == n) { first = i; fx = x; fy = y; }
            else cv_line(x0, y0, x, y, tone(g, 80));
            x0 = x; y0 = y;
        }
        if (first < n && h > 1u) cv_line(x0, y0, fx, fy, tone(g, 80));
        if (cur >= 0) { arp_pt((uint32_t)cur, n, 42, &x, &y); cv_line(120, OB_H / 2, x, y, tone(g, 70)); }   /* the hand */
        for (i = 0; i < n; i++) {               /* the steps */
            int hit = euclid(i, n, h, ro);
            arp_pt(i, n, 50, &x, &y);
            if (i == 0u) { arp_pt(i, n, 58, &x0, &y0); cv_rect(x0 - 1, y0 - 1, 3, 3, tone(g, 150)); }   /* the accent */
            if ((int32_t)i == cur && hit && fl) cv_rect(x - 4, y - 4, 9, 9, tone(g, 255));
            else if (hit) cv_rect(x - 3, y - 3, 7, 7, tone(g, (int32_t)i == cur ? 235 : 60 + (int32_t)o->chc * 5));
            else cv_rect(x - 1, y - 1, 3, 3, tone(g, (int32_t)i == cur ? 200 : 58));
        }
    }
    cv_blit(0, OB_Y);
}
static void draw_spell(uint32_t cpu, int glow)  /* GLO, SEL: the spell, its scale, the chord sounding, the CPU */
{
    char s[24], *p = s;
    const felucca_font_t *f = text_w(&FONT_L, SPELL[spell_now].name) <= 236 ? &FONT_L : &FONT_S;
    cv_begin(240, OB_H, C_BLACK);
    cv_text((240 - text_w(f, SPELL[spell_now].name)) / 2, 14, f, SPELL[spell_now].name, glow ? RGB(240, 215, 170) : C_ACC);
    put(&p, PC_NAME[scale_pc[0]]); *p++ = ' '; put(&p, MODE_NAME[SPELL[spell_now].mode[scale_now]]); *p = 0;
    cv_text((240 - text_w(&FONT_S, s)) / 2, 56, &FONT_S, s, glow ? RGB(240, 215, 170) : C_ACC);
    p = s;
    if (run) { put(&p, PC_NAME[cpc(chord_now, 0)]); put(&p, chord_qual(chord_now)); }
    *p = 0;
    cv_text((240 - text_w(&FONT_S, s)) / 2, 78, &FONT_S, s, C_OFF);
    p = s; put(&p, "CPU ");
    if (cpu) { put_num(&p, cpu); *p++ = '%'; } else put(&p, "--");
    *p = 0;
    cv_text((240 - text_w(&FONT_S, s)) / 2, 102, &FONT_S, s, RGB(60, 60, 60));
    cv_blit(0, OB_Y);
}
static void fmt_season(char **p, uint32_t sea)   /* OFF, 45s, 1m, 1m15 */
{
    uint32_t t = SEASON_S(sea);
    if (!t) { put(p, "OFF"); return; }
    if (t < 60u) { put_num(p, t); *(*p)++ = 's'; return; }
    put_num(p, t / 60u); *(*p)++ = 'm';
    if (t % 60u) put_num(p, t % 60u);
}
static void fmt_field(char **q, uint32_t f)     /* a setting's value as the panel shows it */
{
    uint32_t v = f < F_N ? *fp(f) : 0u, ix;
    switch (f) {
    case F_CHG: if (CHG_N[v]) put_num(q, CHG_N[v]); else put(q, "HOLD"); break;
    case F_PUL: put(q, PULSE_LABEL[v]); break;
    case F_SHP: put(q, SHAPE_NAME[v]); break;
    case F_SEA: case F_ROO: fmt_season(q, v); break;
    case F_CUT: put(q, CUT_LABEL[v]); break;
    case F_SPC: put(q, SPACE_LABEL[v]); break;
    case F_MIX: case F_CHC: case F_LVL: case F_WAN: put_num(q, v * 5u); *(*q)++ = '%'; break;
    case F_TNC: put(q, TONIC_NAME[v]); break;
    case F_GRP: put(q, GRP_FIND[v]); break;
    case F_SCL: put_num(q, v + 1u); *(*q)++ = '/'; put_num(q, SPELL[set.spl].n); break;
    case F_LFT: put(q, v ? fname(LFO_F[v]) : "OFF"); break;
    case F_LFR: put(q, PULSE_LABEL[v]); break;
    case F_LFD: put_num(q, v * 5u); *(*q)++ = '%'; break;
    case F_LFS: put(q, LFS_NAME[v]); break;
    case F_HOL:
        if (CUR->grp != G_PLANT) { put_num(q, v); break; }
        if (v != 10u) *(*q)++ = v < 10u ? '-' : '+';   /* a plant's BEND: down or up from the middle */
        put_num(q, v < 10u ? 10u - v : v - 10u);
        break;
    case F_HIT: put_num(q, v < CUR->stp ? v : CUR->stp); break;
    case F_RAT: put(q, RATE_LABEL[v]); break;
    case F_ATT: put_ms(q, ATT_MS[v]); break;
    case F_HLD: put_ms(q, HLD_MS[v]); break;
    case F_DEC: ix = DEC0 + v; put_ms(q, DK_MS[ix]); break;
    case F_NONE: break;
    default: put_num(q, v); break;
    }
}
static void draw_knobs(uint32_t page)           /* the page's four knobs: a name, its value, a bar */
{
    uint32_t k;
    cv_begin(240, 50, C_BLACK);
    for (k = 0; k < 4u; k++) {
        uint32_t f = page_field(page, k), x0 = k * 60u;
        char v[16], *q = v;
        if (f == F_NONE) continue;              /* (an empty knob) */
        fmt_field(&q, f);
        *q = 0;
        cv_text((int32_t)x0 + (60 - text_w(&FONT_S, fname(f))) / 2, 0, &FONT_S, fname(f), C_OFF);
        cv_text((int32_t)x0 + (60 - text_w(&FONT_S, v)) / 2, 18, &FONT_S, v, C_ACC);
        if (cfg_max(f) > CFG_MIN[f]) {
            uint32_t val = *fp(f) > cfg_max(f) ? cfg_max(f) : *fp(f);
            uint32_t w = (val - CFG_MIN[f]) * 46u / (cfg_max(f) - CFG_MIN[f]);
            cv_rect((int32_t)x0 + 7, 40, 46, 3, RGB(45, 45, 45));
            if (w) cv_rect((int32_t)x0 + 7, 40, (int32_t)w, 3, C_ACC);
        }
    }
    cv_blit(0, 168);
}
/* words for a moment */
static char say_s[2][20];                       /* [0] left, the harmony; [1] right, the rest */
static uint32_t say_t[2], say_v[2];
static void say_at(uint32_t side, const char *s)
{
    uint32_t i = 0;
    while (s[i] && i < sizeof say_s[0] - 1u) { say_s[side][i] = s[i]; i++; }
    say_s[side][i] = 0;
    say_t[side] = fm1_ms; say_v[side]++;
}
static void say(const char *s) { say_at(1u, s); }   /* (on the right) */
static void say_field(uint32_t f)
{
    char s[34], *q = s;
    put(&q, fname(f)); *q++ = ' '; fmt_field(&q, f);
    *q = 0;
    say(s);
}
static void say_scale(void)
{
    char s[24], *p = s;
    put(&p, PC_NAME[scale_pc[0]]); *p++ = ' '; put(&p, MODE_NAME[SPELL[spell_now].mode[scale_now]]);
    *p = 0;
    say_at(0u, s);
}

/* turning things */
static int field_turn(uint32_t f, int32_t e)    /* a setting turned e detents; 1: the tonic or scale changed */
{
    int32_t d = (int32_t)*fp(f) + e;
    *fp(f) = (uint8_t)(d < (int32_t)CFG_MIN[f] ? (int32_t)CFG_MIN[f] : d > (int32_t)cfg_max(f) ? (int32_t)cfg_max(f) : d);
    if (f == F_STP) {                           /* fewer steps: hits and rotation stay inside */
        if (CUR->hit > CUR->stp) CUR->hit = CUR->stp;
        if (CUR->rot >= CUR->stp) CUR->rot = (uint8_t)(CUR->stp - 1u);
    }
    if (f == F_TNC || f == F_SCL) cast(set.spl, set.tnc, set.scl);
    apply();
    return f == F_TNC || f == F_SCL;
}
static void kit_turn(int32_t e)                 /* ALGORITHM: the next (or last) object there is, the hand among them */
{
    uint32_t i, c = set.cur;
    for (i = 0; i <= NKIT; i++) {
        c = (c + (e > 0 ? 1u : NKIT)) % (NKIT + 1u);
        if (set.kit[c].on) break;
    }
    if (set.kit[c].on) set.cur = (uint8_t)c;
}
static void say_cur(void)                       /* "ROCK 3", "STICK NEW" */
{
    char s[20], *q = s;
    if (!CUR->on) { say("EMPTY"); return; }
    put(&q, GRP_NAME[CUR->grp]); *q++ = ' ';
    if (set.cur < NKIT) put_num(&q, set.cur + 1u); else put(&q, "NEW");
    *q = 0;
    say(s);
}

/* ---- MIDI in: from the main loop, and from app_idle while the screen draws (idle_ok) */
static uint32_t idle_ok;
static void midi_in(void)
{
    uint8_t st, d1, d2;
    while (midi_get(&st, &d1, &d2) != 0) {      /* 1 USB, 2 the TRS jack */
        uint32_t ty = st & 0xF0u;
        if (st >= 0xF8u) continue;              /* (clock and transport: not yet) */
        if (set.ch && (st & 15u) != set.ch - 1u) continue;      /* not our channel */
        if (ty == 0x90u && d2) strike(set.cur, d1, d2);
    }
}
static void app_idle(void)                      /* while the screen draws: MIDI in, and the cycles keep their time */
{
    if (!idle_ok) return;
    midi_in();
    cycle_tick();
}

static void app_main(void)
{
    static obj_t before;                        /* the object that was in hand before the last find (REC held: back) */
    uint32_t prev = 0, k, t_change = 0, dirty = 0, shown = NPAGE, page = P_HOME, cpu_pct = 0, turn_ms = 0, st;
    uint32_t top_drawn = ~0u, pic_drawn = ~0u, pic_t = 0, kit_drawn = ~0u, knobs_drawn = ~0u, label_drawn = ~0u, label2_drawn = ~0u;
    uint32_t rec_t = 0, rec_done = 1, save_t = 0, save_done = 1;
#ifndef DINGHY_HOST
    uint32_t cpu_t = 0;
#endif
    uint8_t held[27] = {0};                     /* the note each key plays */
    static uint8_t was_hit[NKIT + 1u];          /* F#5: the hits each place had before it fell silent */
#if BOARD_SMK37
    smk_pad_events = 1;                         /* the pads as events (smk_pad_take): they play here */
#endif
    {                                           /* the settings: loaded, every one in its range, else all the defaults */
        static uint8_t old[F_G + (NKIT + 1u) * 20u];   /* the layout before the objects had an LFO: 20 bytes an object */
        int ok = !settings_load(&set, sizeof set);
        if (!ok && !settings_load(old, sizeof old)) {   /* that one: carried over, the kit kept, its LFOs off */
            for (k = 0; k < F_G; k++) ((uint8_t *)&set)[k] = old[k];
            for (k = 0; k <= NKIT; k++) {
                uint32_t i;
                for (i = 0; i < 20u; i++) ((uint8_t *)&set.kit[k])[i] = old[F_G + k * 20u + i];
                set.kit[k].lft = 0; set.kit[k].lfr = 6; set.kit[k].lfd = 8; set.kit[k].lfs = 2;
            }
            ok = 1;
        }
        ok = ok && (int8_t)set.oct >= -2 && (int8_t)set.oct <= 2;
        for (k = 1; ok && k < F_G; k++) ok = *fp(k) >= CFG_MIN[k] && *fp(k) <= CFG_MAX[k];
        if (ok) ok = set.scl < SPELL[set.spl].n;
        for (k = 0; ok && k <= NKIT; k++) {
            const obj_t *o = &set.kit[k];
            uint32_t f;
            if (!o->on) continue;
            ok = o->on == 1u && o->grp < NGRP && o->hit <= o->stp && o->rot < o->stp;
            for (f = F_SIZ; ok && f < F_N; f++) ok = *obj_fp(o, f) >= CFG_MIN[f] && *obj_fp(o, f) <= CFG_MAX[f];
        }
        if (!ok) {
            for (k = 0; k < sizeof set; k++) ((uint8_t *)&set)[k] = 0;
            for (k = 0; k < F_G; k++) *fp(k) = SET_DEF[k];
            find(HAND, G_ROCK);                 /* (a first rock in hand: the keys sound at once) */
        }
    }
    spell_now = set.spl; tonic = set.tnc; scale_now = set.scl;
    scale_load();
    apply();
    lcd_fill(0, 0, 240, 240, C_BLACK);

    for (;;) {
        uint32_t keys, down, up, rel, b, lit = 0, out_ch = set.ch ? set.ch - 1u : 0u, kl, now;
        int32_t oct = (int8_t)set.oct, e;
        uint32_t base = (uint32_t)((int32_t)KEY0 + 12 * oct);
        platform_poll();

        keys = fm1_in.notes;                    /* bit k: key k (0 = F3 .. 26 = G5) */
        down = keys & ~prev;
        up = prev & ~keys;
        prev = keys;
        for (k = 0; k < 27u; k++) {             /* the keys: the object in hand, in the scale */
#if BOARD_SMK37
            uint32_t vel = fm1_in.key_vel[SMK_KEY_F3 + k];   /* the SMK's keys feel the velocity */
#else
            uint32_t vel = 100;
#endif
            if (up >> k & 1u && held[k]) { midi_send(0x80u | out_ch, held[k], 0); held[k] = 0; }
            if (!(down >> k & 1u)) continue;
            if (k == KEY_MUTE) {                /* F#5, on every page: the object's hits to 0; again: what they were */
                if (!CUR->on) continue;
                if (CUR->hit) { was_hit[set.cur] = CUR->hit; CUR->hit = 0; say("SILENT"); }
                else { CUR->hit = was_hit[set.cur] && was_hit[set.cur] <= CUR->stp ? was_hit[set.cur] : 1u; say_field(F_HIT); }
                dirty = 1; t_change = fm1_ms;
            } else if (page == P_ARP1 || page == P_ARP2) {   /* ARP: the white keys set the hits, 1 .. 16 (the knob goes on) */
                uint32_t w;
                for (w = 0; w < 16u && WHITE[w] != k; w++) ;
                if (w < 16u && CUR->on) { CUR->hit = (uint8_t)(w + 1u > CUR->stp ? CUR->stp : w + 1u); dirty = 1; t_change = fm1_ms; }
            } else { held[k] = (uint8_t)degrees(base + k, 0); strike(set.cur, held[k], vel); midi_send(0x90u | out_ch, held[k], vel); }
        }
#if BOARD_SMK37
        while ((e = smk_extkey_take()) >= 0) {  /* the keys left and right of the FM-1's 27: key << 8 | velocity */
            uint32_t key = (uint32_t)e >> 8, vel = (uint32_t)e & 0x7Fu;
            if (key < FM1_NKEY && vel) strike(set.cur, base + key - SMK_KEY_F3, vel);
        }
        while ((e = smk_pad_take()) >= 0) {     /* a pad: hardware pad << 8 | velocity (0 = let go) */
            uint32_t pad = ((uint32_t)e >> 8) & 15u, vel = (uint32_t)e & 0x7Fu;
            if (vel) strike(set.cur, base + WHITE[pad < 8u ? pad + 8u : pad - 8u], vel);
        }
#endif

        b = fm1_input_edges(&rel);              /* buttons pressed since the last call (bit = button id) */
        if (b & (1u << BTN_OCT_DOWN) && oct > -2) { set.oct = (uint8_t)(oct - 1); dirty = 1; t_change = fm1_ms; }
        if (b & (1u << BTN_OCT_UP) && oct < 2) { set.oct = (uint8_t)(oct + 1); dirty = 1; t_change = fm1_ms; }
        if (b & (1u << BTN_HOME)) page = P_HOME;
        if (b & (1u << BTN_ENV)) page = page == P_ENV ? P_HOME : P_ENV;
        if (b & (1u << BTN_ARP)) page = page == P_ARP1 ? P_ARP2 : P_ARP1;   /* ARP: the cycle; again: chance, wander */
        if (b & (1u << BTN_FX)) page = page == P_FX ? P_HOME : P_FX;
        if (b & (1u << BTN_GLO)) page = page == P_GLO ? P_HOME : P_GLO;
        if (b & (1u << BTN_SEL)) page = page == P_SEL ? P_HOME : P_SEL;
        if (b & (1u << BTN_LFO)) page = page == P_LFO ? P_HOME : P_LFO;   /* LFO: the object's own */
        if (b & (1u << BTN_REC)) { rec_t = fm1_ms; rec_done = 0; }   /* REC: a tap finds an object; held: the one before */
        if (fm1_in.buttons >> BTN_REC & 1u && !rec_done && fm1_ms - rec_t > 600u) {
            rec_done = 1;
            if (before.on) { obj_t t = *HAND; *HAND = before; before = t; set.cur = NKIT; say("BACK"); dirty = 1; t_change = fm1_ms; }
        }
        if (rel & (1u << BTN_REC) && !rec_done) {
            rec_done = 1;
            before = *HAND;
            find(HAND, set.grp);
            set.cur = NKIT;
            say_cur(); dirty = 1; t_change = fm1_ms;
        }
        if (b & (1u << BTN_SAVE)) { save_t = fm1_ms; save_done = 0; }   /* SAVE: a tap keeps the hand's object; held: a kit object out */
        if (fm1_in.buttons >> BTN_SAVE & 1u && !save_done && fm1_ms - save_t > 600u) {
            save_done = 1;
            if (set.cur < NKIT && CUR->on) {
                before = *HAND; *HAND = *CUR; CUR->on = 0; set.cur = NKIT;
                say("OUT OF KIT"); dirty = 1; t_change = fm1_ms;
            }
        }
        if (rel & (1u << BTN_SAVE) && !save_done) {
            save_done = 1;
            if (set.cur == NKIT && HAND->on) {
                for (k = 0; k < NKIT && set.kit[k].on; k++) ;
                if (k == NKIT) say("KIT FULL");
                else { set.kit[k] = *HAND; HAND->on = 0; set.cur = (uint8_t)k; say_cur(); dirty = 1; t_change = fm1_ms; }
            } else if (CUR->on) say("IN THE KIT");
        }
        if ((e = fm1_enc_take(ENC_SELECT)) != 0) {     /* SELECT: the spell, round the book */
            set.spl = (uint8_t)(((int32_t)set.spl + e % (int32_t)NSPELL + (int32_t)NSPELL) % (int32_t)NSPELL);
            set.scl = 0;
            cast(set.spl, set.tnc, 0);
            say_at(0u, SPELL[spell_now].name);
            turn_ms = fm1_ms; dirty = 1; t_change = fm1_ms;
        }
        if ((e = fm1_enc_take(ENC_PRESETS)) != 0) {    /* PRESETS: the kind REC finds, round the four */
            set.grp = (uint8_t)(((int32_t)set.grp + e % (int32_t)NGRP + (int32_t)NGRP) % (int32_t)NGRP);
            say(GRP_FIND[set.grp]);
            dirty = 1; t_change = fm1_ms;
        }
        if ((e = fm1_enc_take(ENC_ALGORITHM)) != 0) {  /* ALGORITHM: round the kit */
            kit_turn(e); say_cur(); dirty = 1; t_change = fm1_ms;
        }
        for (k = 0; k < 4u; k++)                /* KNOB 1..4: the page's four settings */
            if ((e = fm1_enc_take(ENC_KNOB1 + k)) != 0) {
                uint32_t f = page_field(page, k);
                if (f >= F_N) continue;         /* (an empty knob) */
                if (field_turn(f, e)) { turn_ms = fm1_ms; say_scale(); }
                if (page == P_HOME) say_field(f);
                dirty = 1; t_change = fm1_ms;
            }

        if (b & (1u << BTN_PLAY)) {             /* PLAY: the cycles run, from their first steps; again: they stop */
            if (run) run = 0;
            else {
                grow_rng ^= fm1_ms * 2654435761u;
                if (!grow_rng) grow_rng = 1;
                chord_now = 0; homing = 0; moves_left = new_moves();
                lfo_reset();
                tick_next = fm1_ms << 4; tick_n = 0;
                run = 1;
            }
        }
        if (lfo_tick(set.bpm, set.pul, set.shp, set.sea, set.roo)) {   /* the season or ROOTS turned */
            set.scl = (uint8_t)scale_now;
            set.tnc = (uint8_t)tonic;
            dirty = 1; t_change = fm1_ms; turn_ms = fm1_ms;
            say_scale();
        }
        cycle_tick();
        midi_in();

        now = fm1_ms;
        if (page == P_ARP1 || page == P_ARP2) { /* ARP: the white keys show the hits; elsewhere a key is lit while held */
            for (k = 0; k < 16u; k++) if (CUR->on && k < CUR->hit) lit |= 1u << WHITE[k];
        } else lit = keys & ~(1u << KEY_MUTE);
        if (CUR->on && !CUR->hit) lit |= 1u << KEY_MUTE;   /* F#5 lit: silent */
        for (k = 0; k < 27u; k++) fm1_led_key(KEY_LED(k), lit >> k & 1u);
        fm1_led_key(BTN_HOME, page == P_HOME);  /* the page's button lit */
        fm1_led_key(BTN_ENV, page == P_ENV);
        fm1_led_key(BTN_ARP, page == P_ARP1 || page == P_ARP2);
        fm1_led_key(BTN_FX, page == P_FX);
        fm1_led_key(BTN_GLO, page == P_GLO);
        fm1_led_key(BTN_SEL, page == P_SEL);
        fm1_led_key(BTN_LFO, page == P_LFO);
        fm1_led_key(BTN_REC, fm1_in.buttons >> BTN_REC & 1u);
        fm1_led_key(BTN_SAVE, set.cur == NKIT && HAND->on);   /* lit: the object in hand is not kept yet */
        fm1_led_key(BTN_OCT_DOWN, oct < 0);
        fm1_led_key(BTN_OCT_UP, oct > 0);
        fm1_led_key(BTN_PLAY, run && !(tick_n % 24u < 3u));   /* lit: running; a blink a beat */

        idle_ok = 1;                            /* the drawing below may read MIDI meanwhile (LCD_IDLE) */
#ifndef DINGHY_HOST
        if (fm1_ms - cpu_t >= 1000u) {          /* the audio ISR's worst run of the last second, of its 5.8 ms */
            cpu_t = fm1_ms;
            cpu_pct = cpu_max_us * 100u / 5805u;
            cpu_max_us = 0;
        }
#endif
        kl = turn_ms && now - turn_ms < 3000u;  /* the scale just turned: it glows */
        if ((page == P_HOME) != (shown == P_HOME) || shown == NPAGE) {   /* HOME and the other pages: each its own screen */
            lcd_fill(0, 0, 240, 240, C_BLACK);
            top_drawn = pic_drawn = kit_drawn = knobs_drawn = label_drawn = label2_drawn = ~0u;
        }
        shown = page;
        if ((st = set.bpm | page << 8 | (uint32_t)set.cur << 12 | (uint32_t)CUR->on << 16 | (uint32_t)CUR->grp << 17 | (uint32_t)set.grp << 20) != top_drawn) {
            char c[16], *q = c;                 /* the top line: the tempo; the kind (HOME) or the page; the place in the kit */
            top_drawn = st;
            put_num(&q, set.bpm); put(&q, " BPM"); *q = 0;
            if (page == P_HOME) draw_find();    /* HOME: what REC finds, in place of the tempo (that is on GLO) */
            else draw_text_box(4, 4, 68, &FONT_S, c, C_OFF, 0);
            draw_text_box(72, 4, 96, &FONT_S, page != P_HOME ? PAGE_NAME[page] : CUR->on ? GRP_NAME[CUR->grp] : "", page == P_HOME && CUR->on ? tone(CUR->grp, 160) : C_ACC, 1);
            q = c;
            if (set.cur < NKIT) { put_num(&q, set.cur + 1u); put(&q, "/8"); } else put(&q, CUR->on ? "NEW" : "");
            *q = 0;
            draw_text_box(168, 4, 68, &FONT_S, c, set.cur < NKIT ? C_OFF : C_ACC, 2);
        }
        if (now - pic_t >= 40u) {               /* the picture: at most 25 a second */
            int fl = hit_ms[set.cur] && now - hit_ms[set.cur] < 110u;
            if (page == P_ARP1 || page == P_ARP2) {
                st = hsh(obj_seed(CUR) ^ (uint32_t)CUR->stp << 8 ^ eff(set.cur, F_HIT) << 14 ^ eff(set.cur, F_ROT) << 20 ^ (uint32_t)CUR->chc << 26)
                     + (run ? 1000u + cyc_step(CUR) : 0u) + (uint32_t)(run && hit_ms[set.cur] && now - hit_ms[set.cur] < 200u) * 77u + page * 5u + CUR->on;
                if (st != pic_drawn) { pic_drawn = st; pic_t = now; draw_arp(now); }
            } else if (page == P_ENV) {
                st = 0x60000000u + CUR->att + CUR->hld * 16u + CUR->dec * 256u + CUR->lvl * 16384u + CUR->grp * 0x80000u + CUR->on * 0x400000u + set.cur * 0x800000u;
                if (st != pic_drawn) { pic_drawn = st; pic_t = now; draw_env(CUR); }
            } else if (page == P_LFO) {
                st = 0x70000000u + CUR->lft + CUR->lfr * 16u + CUR->lfd * 256u + CUR->lfs * 8192u + CUR->on * 32768u + set.cur * 65536u + (obj_seed(CUR) & 0xF00000u)
                     + (CUR->lft && CUR->lfs != 2u ? ((run ? tick_n : fm1_ms * set.bpm / 2500u) % (PULSE_Q[CUR->lfr] * 12u) * 200u / (PULSE_Q[CUR->lfr] * 12u)) << 24 : 0u);
                if (st != pic_drawn) { pic_drawn = st; pic_t = now; draw_lfo(set.cur); }
            } else if (page == P_GLO || page == P_SEL) {
                st = 0x50000000u + spell_now + scale_now * 8u + tonic * 128u + (run ? 2048u + chord_now * 4096u : 0u) + cpu_pct * 65536u + kl * 0x1000000u;
                if (st != pic_drawn) { pic_drawn = st; pic_t = now; draw_spell(cpu_pct, (int)kl); }
            } else {
                st = hsh(obj_seed(CUR) + CUR->siz + CUR->mat * 32u + CUR->hol * 1024u + CUR->stk * 32768u + CUR->grp * 0x100000u) + (uint32_t)fl + CUR->on * 2u;
                if (st != pic_drawn) { pic_drawn = st; pic_t = now; draw_object(CUR, fl); }
            }
        }
        if (page == P_HOME) {
            uint32_t lv = 0;
            const char *ls = "";
            st = set.cur;                       /* the kit: redrawn when it changes or a place sounds */
            for (k = 0; k < NKIT; k++) st = st * 11u + set.kit[k].on * (1u + set.kit[k].grp + (set.kit[k].hit ? 0u : 5u)) + (uint32_t)(hit_ms[k] && now - hit_ms[k] < 110u) * 4u;
            if (st != kit_drawn) { kit_drawn = st; draw_kit(now); }
            if (say_v[0] && now - say_t[0] < 1800u) { ls = say_s[0]; lv = say_v[0]; }   /* left: the harmony, a moment */
            if (lv != label_drawn) { label_drawn = lv; draw_text_box(6, 216, 114, &FONT_S, ls, C_ACC, 0); }
            lv = 0; ls = "";
            if (say_v[1] && now - say_t[1] < 1800u) { ls = say_s[1]; lv = say_v[1]; }   /* right: what was just turned */
            if (lv != label2_drawn) { label2_drawn = lv; draw_text_box(120, 216, 114, &FONT_S, ls, C_ACC, 2); }
        } else {
            st = page * 131u + set.cur;         /* the panel: redrawn when the page or a value on it changes */
            for (k = 0; k < 4u; k++) { uint32_t f = page_field(page, k); st = st * 31u + (f < F_N ? *fp(f) : 0u) + f * 7u; }
            if (st != knobs_drawn) { knobs_drawn = st; draw_knobs(page); }
        }
        idle_ok = 0;

        if (dirty && quiet() && fm1_ms - t_change > 2000u) {   /* 2 s after the change, when silent (an erase stalls the sound) */
            settings_save(&set, sizeof set);
            dirty = 0;
        }
    }
}
