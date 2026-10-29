/*
 * Test controls for the two crowds: making a pinned fight's pictures repeat.
 *
 * docs/research/crowd-nondeterminism.md found that with every seed pinned, a fight's
 * fighters, HUD and arena repeat pixel for pixel but the crowds do not, because
 * both start animating while the match loads and leave that window in a slightly
 * different state each run:
 *
 *  - the background crowd (sub_000DB870, run when the byte at 0x2E3488 is set) is
 *    updated 902 times in one run's loading window and 903 in another's, and each
 *    update draws from the C runtime's rand, so every later draw is shifted;
 *  - the foreground spectators draw their animation variants from the generator
 *    at 0x375F68 a different number of times in the same window.
 *
 * Controls, selected with RECOMP_TEST_CROWD (comma-separated), only while the test
 * probes are on (RECOMP_TEST_OBSERVATIONS=1). `hold,spect,reseed` together make a
 * pinned full match repeat step for step (2026-10-28, two pairs to the result):
 *
 *   hold[:N]   let the background crowd update N times after match set-up (default
 *              600; the window had 902 in 2026-10 builds and about 680 since), then
 *              clear 0x2E3488 so it holds still until the first fight step, where
 *              the byte is put back, with the time it gathered meanwhile (+0x13C)
 *              cleared. Every run then enters the fight with the crowd updated
 *              exactly N times. (Clearing the byte for the whole window crashed at
 *              the first step, because the first updates also set up the crowd's
 *              drawing; N updates do that first.)
 *   spect[:M]  the spectators: their manager's update (sub_00081170) is deferred
 *              out of the window and run M times (default 500; about 624 happen in
 *              a window) at the first fight step, once everything has loaded. Its
 *              dt is a fixed 1/60 and the time scale at 0x3BDE80 is 0 while the
 *              match loads, so time does not move them; but each call visits the
 *              next of the 30 spectators (a rotor at +0x1C) and picks animations
 *              for those whose data has arrived, which follows the host's loading
 *              speed. In the fight it runs once a fight step instead of once a
 *              frame, and the spectators draw from a private copy of the shared
 *              generator at 0x33DB5C. See defjam_crowd_spectators.
 *   reseed     pin CLogic's generator (0x376FDC, seeded from the time-stamp counter)
 *              where it is seeded, and at the first fight step re-seed every
 *              object-form generator that
 *              was seeded during set-up (0x375F68, 0x376FDC, ...), through the
 *              game's own seeding routine sub_000AA0D0, from RECOMP_TEST_RNG_SEED.
 *              Whatever the window drew, the spectators' draws from then on are
 *              the same in every run. It changes the fight's state stream (one of
 *              these generators feeds the fight), so a stream recorded with it on
 *              is only comparable with runs that also have it on.
 *
 *   probe      log the spectators' updates, the background crowd's time and every
 *              draw in the window, to line two runs up draw by draw.
 *
 * scripts/crowd-lab.py runs the older matrix (none, hold, reseed, both, and
 * RECOMP_WORKERS=inline); scripts/sync_diff.py on two RECOMP_TEST_SYNC=1 runs is the
 * test that a whole match repeats. Each action is logged as [TEST-CROWD].
 */

#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "guest_call.h"
#include "recorder.h"
#include "test_observations.h"

extern void sub_000AA0D0(void);

#define CROWD_GATE      0x002E3488u
#define SEEDS_MAX       8

static int s_parsed;
static int s_hold, s_hold_n = 600, s_reseed, s_probe;
static int s_spect, s_spect_n = 500;
static long s_spect_calls;          /* spectator updates in the window */
static uint32_t s_spect_manager, s_crowd_object;    /* last seen, for the hashes */
static long s_updates;              /* the crowd updates on the game thread */
static int s_in_window, s_held, s_gate_value;
static struct { uint32_t generator, high, low; } s_seeds[SEEDS_MAX];
static int s_seed_count;

static void parse(void)
{
    const char *spec = getenv("RECOMP_TEST_CROWD");
    s_parsed = 1;
    if (!spec || !*spec || !defjam_test_observations_enabled())
        return;
    s_probe = strstr(spec, "probe") != NULL;
    s_hold = strstr(spec, "hold") != NULL;
    s_reseed = strstr(spec, "reseed") != NULL;
    s_spect = strstr(spec, "spect") != NULL;
    {
        const char *n = strstr(spec, "hold:");
        if (n && atoi(n + 5) > 0)
            s_hold_n = atoi(n + 5);
        n = strstr(spec, "spect:");
        if (n && atoi(n + 6) > 0)
            s_spect_n = atoi(n + 6);
    }
    fprintf(stderr, "[TEST-CROWD] {\"event\":\"armed\",\"hold\":%s,\"hold_updates\":%d,\"spect\":%s,"
                    "\"spect_updates\":%d,\"reseed\":%s}\n",
            s_hold ? "true" : "false", s_hold_n, s_spect ? "true" : "false", s_spect_n,
            s_reseed ? "true" : "false");
}

static int active(void)
{
    if (!s_parsed) parse();
    return s_hold || s_reseed || s_probe || s_spect;
}

/* sub_00081170(this, dt): the spectators' update, the third entry of their manager's
 * vtable (0x28D9B0 in the XBE). Returns 1 when the call is to be skipped. `probe`
 * logs each call in the window and the first calls of the fight.
 *
 * Holding the count was not enough: with exactly M calls in two runs, the same call
 * made different animation picks, because a spectator only picks once its animation
 * data is in, and that arrives from disc at a host-timed point of the window. So
 * `spect` lets the first SPECT_SETUP calls run (they set the spectators up), skips
 * the rest of the window, and at the first fight step, when everything has loaded,
 * runs the M updates back to back with the loading-time scale (0x3BDE80 = 0).
 *
 * In the fight the update runs once a presented frame, and the fighters once a fight
 * step; a frame that catches up runs two steps and one update, so the spectators
 * drifted from step 1 on (112 and 116 draws by step 300). With `spect` the frame's
 * call is skipped from the first step until the next match is set up, and
 * defjam_crowd_step runs the update once a step instead. */
#define SPECT_SETUP 4
extern void sub_00081170_gen(void);

/* The spectators also draw from a global add-with-carry generator, six words at
 * 0x33DB5C (sub_0002A390, through sub_0002A770): their speeds at set-up
 * (sub_00080ED0: 1 + ((draw % 60) - 30) * k, measured 1.02 against 1.00 for the same
 * spectator) and their choices in every update (sub_00080A00). 236 sites draw from it,
 * menus among them, so its state when the spectators use it follows the host. With
 * `spect` they get a private copy, seeded at their set-up from RECOMP_TEST_RNG_SEED and
 * swapped in around every set-up and update call. */
#define SHARED_RNG      0x0033DB5Cu
#define SHARED_RNG_WORDS 6
static uint32_t s_private_rng[SHARED_RNG_WORDS], s_saved_rng[SHARED_RNG_WORDS];
static int s_private_seeded;

int defjam_crowd_rng_swap(int enter)
{
    int i;
    if (!active() || !s_spect || !s_private_seeded) return 0;
    for (i = 0; i < SHARED_RNG_WORDS; i++) {
        uint32_t addr = SHARED_RNG + (uint32_t)i * 4;
        if (enter) {
            s_saved_rng[i] = MEM32(addr);
            MEM32(addr) = s_private_rng[i];
        } else {
            s_private_rng[i] = MEM32(addr);
            MEM32(addr) = s_saved_rng[i];
        }
    }
    return 1;
}

/* sub_00081240: the spectator manager's set-up (second vtable entry). */
void defjam_crowd_spectator_setup(void)
{
    const char *seed_text;
    uint32_t seed;
    int i;
    if (!active() || !s_spect) return;
    seed_text = getenv("RECOMP_TEST_RNG_SEED");
    seed = seed_text && *seed_text ? (uint32_t)strtoul(seed_text, NULL, 0) : 0x5EED;
    for (i = 0; i < SHARED_RNG_WORDS - 1; i++) {
        seed = seed * 1664525u + 1013904223u;
        s_private_rng[i] = seed;
    }
    s_private_rng[SHARED_RNG_WORDS - 1] = 0;    /* +0x14 counts draws */
    s_private_seeded = 1;
    fprintf(stderr, "[TEST-CROWD] {\"event\":\"spect_rng_seeded\"}\n");
}

static void spectator_update(uint32_t self, uint32_t *dt)
{
    int swapped = defjam_crowd_rng_swap(1);
    guest_call(sub_00081170_gen, self, 1, dt);
    if (swapped) defjam_crowd_rng_swap(0);
}

static uint32_t s_spect_self, s_spect_dt;
static int s_spect_fight;           /* the fight's steps drive the spectators */

int defjam_crowd_spectators(uint32_t self, uint32_t stack)
{
    static long calls_in_fight;
    s_spect_manager = self;
    if (!active()) return 0;
    if (s_spect && (s_spect_fight || (s_in_window && ++s_spect_calls > SPECT_SETUP))) {
        if (s_in_window && s_spect_calls == SPECT_SETUP + 1)
            fprintf(stderr, "[TEST-CROWD] {\"event\":\"spect_defer\",\"after\":%d}\n", SPECT_SETUP);
        s_spect_self = self;
        s_spect_dt = MEM32(stack + 4);
        return 1;
    }
    if (!s_probe || (!s_in_window && ++calls_in_fight > 120)) return 0;
    fprintf(stderr, "[TEST-CROWD] {\"event\":\"spectators\",\"window\":%d,\"crowd_updates\":%ld,\"this\":%u,"
                    "\"caller\":%u,\"dt\":%.9g,\"scale\":%.9g,\"rotor\":%u,\"count\":%u}\n",
            s_in_window, (long)s_updates, self, MEM32(stack), (double)MEMF(stack + 4),
            (double)MEMF(0x003BDE80u), MEM32(self + 0x1C), MEM32(self + 0x18));
    return 0;
}

/* A draw from the C runtime's rand (generator 1) or an object-form generator. `probe`
 * logs each one in the window with the thread and how far the crowds had got, so two
 * runs can be lined up draw by draw. */
void defjam_crowd_draw(uint32_t generator, uint32_t caller)
{
    static HOST_TLS LONG thread;
    static LONG threads;
    if (!active() || !s_probe || !s_in_window) return;
    char chain[160];
    size_t n = 0;
    uint32_t i, found = 0;
    if (!thread) thread = InterlockedIncrement(&threads);
    /* Words on the guest stack that fall in the code section: likely return addresses,
     * innermost first. A heuristic, enough to tell call paths apart. */
    chain[0] = 0;
    for (i = 4; i < 0x200 && found < 8; i += 4) {
        uint32_t word = MEM32(g_esp + i);
        if (word >= 0x00011000u && word < 0x00260000u) {
            n += (size_t)snprintf(chain + n, sizeof chain - n, "%s%u", found ? "," : "", word);
            found++;
        }
    }
    fprintf(stderr, "[TEST-CROWD] {\"event\":\"draw\",\"generator\":%u,\"caller\":%u,\"thread\":%lu,"
                    "\"crowd_updates\":%ld,\"spectator_updates\":%ld,\"state\":%u,\"chain\":[%s]}\n",
            generator, caller, (unsigned long)thread, (long)s_updates, (long)s_spect_calls,
            generator > 1 ? MEM32(generator + 0x80) : 0u, chain);
}

/* sub_000DB620(this, dt): time added to the background crowd's accumulator (+0x13C),
 * from which sub_000DB730 takes whole animation frames. `probe` logs it in the window. */
void defjam_crowd_time(uint32_t self, uint32_t stack)
{
    if (!active() || !s_probe || !s_in_window) return;
    fprintf(stderr, "[TEST-CROWD] {\"event\":\"crowd_time\",\"this\":%u,\"caller\":%u,\"dt\":%.9g,\"acc\":%.9g,"
                    "\"crowd_updates\":%ld}\n",
            self, MEM32(stack), (double)MEMF(stack + 4), (double)MEMF(self + 0x13C), (long)s_updates);
}

/* sub_001A4D90: the match is being set up. The window starts here. */
void defjam_crowd_match_start(void)
{
    if (!active()) return;
    if (s_held) {                       /* a previous match never reached its first step */
        MEM8(CROWD_GATE) = (uint8_t)s_gate_value;
        s_held = 0;
    }
    s_updates = 0;
    s_spect_calls = 0;
    s_spect_fight = 0;
    s_spect_self = 0;
    s_seed_count = 0;
    s_in_window = 1;
}

/* ---- Hashes for the per-step fingerprint (test_telemetry.c, RECOMP_TEST_SYNC) ---- */

static uint32_t fnv_words(uint32_t h, uint32_t base, uint32_t bytes)
{
    uint32_t i;
    for (i = 0; i < bytes; i += 4)
        h = (h ^ MEM32(base + i)) * 16777619u;
    return h;
}

/* The spectator manager's members (pointers at +0xA90, count at +0x18), the first
 * 0x100 bytes of each: enough to see their animation state part. */
uint32_t defjam_crowd_spectator_hash(void)
{
    uint32_t h = 2166136261u, i, count;
    if (!s_spect_manager) return 0;
    count = MEM32(s_spect_manager + 0x18);
    if (count > 64) count = 64;
    for (i = 0; i < count; i++) {
        uint32_t member = MEM32(s_spect_manager + 0xA90 + i * 4);
        if (member) h = fnv_words(h, member, 0x100);
    }
    return h;
}

/* The background crowd's per-member entries (pointer at +0x124, count at +0x11C,
 * four bytes each; the animation frame is the high half). */
uint32_t defjam_crowd_background_hash(void)
{
    uint32_t count;
    if (!s_crowd_object) return 0;
    count = MEM32(s_crowd_object + 0x11C);
    if (count > 4096) count = 4096;
    return fnv_words(2166136261u, MEM32(s_crowd_object + 0x124), count * 4);
}

int defjam_crowd_hash_part(int part, uint32_t *base, uint32_t *bytes)
{
    if (part < 64) {
        if (!s_spect_manager || (uint32_t)part >= MEM32(s_spect_manager + 0x18)) return 0;
        *base = MEM32(s_spect_manager + 0xA90 + (uint32_t)part * 4);
        *bytes = 0x100;
        return *base != 0;
    }
    if (!s_crowd_object) return 0;
    *base = MEM32(s_crowd_object + 0x124);
    *bytes = MEM32(s_crowd_object + 0x11C) * 4;
    if (*bytes > 4096 * 4) *bytes = 4096 * 4;
    return 1;
}

/* sub_000DB870: one update of the background crowd. */
void defjam_crowd_update(void)
{
    long n;
    s_crowd_object = g_ecx;
    if (!active() || !s_in_window) return;
    n = ++s_updates;
    if (s_hold && !s_held && n == s_hold_n) {
        /* This update runs; the caller tests the byte before the next one. */
        s_gate_value = MEM8(CROWD_GATE);
        MEM8(CROWD_GATE) = 0;
        s_held = 1;
        fprintf(stderr, "[TEST-CROWD] {\"event\":\"hold\",\"updates\":%ld}\n", (long)n);
    }
}

/* sub_000AA0D0(this = generator, high, low): remembered while the match is set up. */
void defjam_crowd_seed(uint32_t generator, uint32_t stack)
{
    int i;
    if (!active() || !s_reseed) return;
    /* CLogic's generator (0x376FDC) is seeded from the time-stamp counter in match
     * setup (sub_0008ABF0) and drawn once before the first step, so re-seeding it
     * there is too late: pin its seed where it is made, as the AI seed is. */
    if (generator == 0x00376FDCu) {
        const char *seed_text = getenv("RECOMP_TEST_RNG_SEED");
        uint32_t base = seed_text && *seed_text ? (uint32_t)strtoul(seed_text, NULL, 0) : 0;
        if (base) {
            MEM32(stack + 4) = base >> 16;          /* as the game splits the counter */
            MEM32(stack + 8) = base & 0xFFFFu;
            fprintf(stderr, "[TEST-CROWD] {\"event\":\"pin_seed\",\"generator\":%u,\"high\":%u,\"low\":%u}\n",
                    generator, base >> 16, base & 0xFFFFu);
        }
    }
    if (!s_in_window) return;
    for (i = 0; i < s_seed_count; i++)
        if (s_seeds[i].generator == generator) break;
    if (i == s_seed_count) {
        if (s_seed_count == SEEDS_MAX) return;
        s_seed_count++;
    }
    s_seeds[i].generator = generator;
    s_seeds[i].high = MEM32(stack + 4);
    s_seeds[i].low = MEM32(stack + 8);
}

/* sub_00087700: a fight step is about to run. The first one closes the window. */
void defjam_crowd_step(void)
{
    const char *seed_text;
    uint32_t base;
    int i;
    char fields[224];
    if (!active()) return;
    if (s_spect_fight && s_spect_self)          /* this step's spectator update */
        spectator_update(s_spect_self, &s_spect_dt);
    if (!s_in_window) return;
    s_in_window = 0;
    if (s_held) {
        MEM8(CROWD_GATE) = (uint8_t)s_gate_value;
        s_held = 0;
        /* Time kept adding to the crowd's accumulator (+0x13C, sub_000DB620) while it
         * was held, as long as the host took to load; released as it is, that would
         * become a different number of frames in every run. */
        if (s_crowd_object)
            MEM32(s_crowd_object + 0x13C) = 0;
    }
    if (s_spect && s_spect_self) {
        uint32_t scale = MEM32(0x003BDE80u);
        MEM32(0x003BDE80u) = 0;
        for (i = 0; i < s_spect_n; i++)
            spectator_update(s_spect_self, &s_spect_dt);
        MEM32(0x003BDE80u) = scale;
        fprintf(stderr, "[TEST-CROWD] {\"event\":\"spect_replay\",\"updates\":%d,\"rotor\":%u}\n",
                s_spect_n, MEM32(s_spect_self + 0x1C));
        s_spect_fight = 1;
        spectator_update(s_spect_self, &s_spect_dt);  /* and the first step's */
    }
    snprintf(fields, sizeof fields, "\"updates_in_window\":%ld,\"held\":%s,\"spectator_updates\":%ld,"
             "\"spect_held\":%s,\"reseeded\":%d",
             (long)s_updates, s_hold && s_updates >= s_hold_n ? "true" : "false", (long)s_spect_calls,
             s_spect && s_spect_calls > SPECT_SETUP ? "true" : "false", s_reseed ? s_seed_count : 0);
    fprintf(stderr, "[TEST-CROWD] {\"event\":\"first_step\",%s}\n", fields);
    recorder_event("crowd", fields);
    if (!s_reseed) return;
    seed_text = getenv("RECOMP_TEST_RNG_SEED");
    base = seed_text && *seed_text ? (uint32_t)strtoul(seed_text, NULL, 0) : 0;
    if (!base)
        fprintf(stderr, "[TEST-CROWD] {\"event\":\"reseed_without_seed\",\"note\":\"set RECOMP_TEST_RNG_SEED; "
                        "the set-up seeds are reused and one of them comes from the time-stamp counter\"}\n");
    for (i = 0; i < s_seed_count; i++) {
        uint32_t args[2];
        args[0] = base ? base + (uint32_t)i * 7919u : s_seeds[i].high;
        args[1] = base ? 0x5EEDu + (uint32_t)i : s_seeds[i].low;
        guest_call(sub_000AA0D0, s_seeds[i].generator, 2, args);
        fprintf(stderr, "[TEST-CROWD] {\"event\":\"reseed\",\"generator\":%u,\"high\":%u,\"low\":%u}\n",
                s_seeds[i].generator, args[0], args[1]);
    }
    fflush(stderr);
}
