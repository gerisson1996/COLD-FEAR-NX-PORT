/*
 * prof.c -- sampling profiler for the guest (COLDFEAR_PROF=1).
 *
 * Lifted functions store their address in g_prof_fn on entry
 * (tools/fix_prof_entry.py). Only the thread holding the guest lock runs
 * guest code, so the variable names the function running now, or the last
 * one entered before a host call (kernel, D3D push, MMIO). A host thread
 * reads it every millisecond and, every 10 seconds, prints the 25 functions
 * that were current most often, with their share of the samples. Time spent
 * in a caller after a callee returns is charged to the callee: the list says
 * where the hot loops are, which is the question.
 */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

volatile uint32_t g_prof_fn;

#define PROF_SLOTS 4096
static struct { uint32_t va, n; } s_hist[PROF_SLOTS];
static uint32_t s_samples, s_dropped;

static void prof_add(uint32_t va)
{
    uint32_t h = (va * 2654435761u) & (PROF_SLOTS - 1), i;
    for (i = 0; i < PROF_SLOTS; i++, h = (h + 1) & (PROF_SLOTS - 1)) {
        if (s_hist[h].n && s_hist[h].va != va)
            continue;
        s_hist[h].va = va;
        s_hist[h].n++;
        return;
    }
    s_dropped++;
}

static void prof_report(void)
{
    int shown, i, best;
    fprintf(stderr, "[PROF] %u samples in 10 s, hottest guest functions:\n", s_samples);
    for (shown = 0; shown < 25; shown++) {
        best = -1;
        for (i = 0; i < PROF_SLOTS; i++)
            if (s_hist[i].n && (best < 0 || s_hist[i].n > s_hist[best].n))
                best = i;
        if (best < 0)
            break;
        fprintf(stderr, "[PROF]   %5.1f%%  sub_%08X\n",
                100.0 * s_hist[best].n / (s_samples ? s_samples : 1), s_hist[best].va);
        s_hist[best].n = 0;
    }
    fflush(stderr);
    memset(s_hist, 0, sizeof s_hist);
    s_samples = s_dropped = 0;
}

static void *prof_thread(void *arg)
{
    struct timespec t0, now;
    (void)arg;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        usleep(1000);
        prof_add(g_prof_fn);
        s_samples++;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec - t0.tv_sec >= 10) {
            prof_report();
            t0 = now;
        }
    }
    return NULL;
}

void coldfear_prof_start(void)
{
    const char *e = getenv("COLDFEAR_PROF");
    pthread_t th;
    if (!e || *e != '1')
        return;
    if (pthread_create(&th, NULL, prof_thread, NULL) == 0) {
        pthread_detach(th);
        fprintf(stderr, "[PROF] sampling guest functions every 1 ms\n");
    }
}
