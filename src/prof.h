/*
 * Section profiling (D-4, D-6; Rev 2.14).
 *
 * Measures elapsed time with the DWT cycle counter (150 cycles = 1 us). The
 * figure includes any time interrupts take while the section runs, which is
 * what matters for a real-time budget. Cost: about 10 cycles per
 * PROF_BEGIN/PROF_END pair. Main loop only (single writer).
 *
 *     PROF_BEGIN(PROF_APP_MAG);
 *     mag_handler();
 *     PROF_END(PROF_APP_MAG);
 *
 * PROF_BEGIN and PROF_END must be in the same block. To profile a new
 * section, add an id below (before PROF_COUNT) and its name in prof.c.
 */
#ifndef PROF_H
#define PROF_H

#include <stdint.h>

#include "diag.h"

typedef enum {
    PROF_EPOCH,     /* whole app_poll() of one 1 ms epoch (EX-8); set by main.c */
    PROF_APP_MAG,   /* app_user.c sections                                       */
    PROF_APP_LINK0,
    PROF_APP_LINK1,
    PROF_COUNT
} prof_id_t;

typedef struct {
    uint32_t last;  /* cycles, most recent run */
    uint32_t max;   /* cycles, worst case since boot */
    uint64_t sum;   /* cycles, for the average */
    uint32_t count; /* runs */
} prof_t;

extern prof_t g_prof[PROF_COUNT];
extern const char *const g_prof_name[PROF_COUNT];

static inline void prof_record(prof_id_t id, uint32_t cycles) {
    prof_t *p = &g_prof[id];
    p->last = cycles;
    if (cycles > p->max) {
        p->max = cycles;
    }
    p->sum += cycles;
    p->count++;
}

/* Average in cycles (0 if never run). */
uint32_t prof_avg(prof_id_t id);

#define PROF_BEGIN(id) const uint32_t prof_t0_##id = diag_cycles()
#define PROF_END(id) prof_record((id), diag_cycles() - prof_t0_##id)

#endif /* PROF_H */
