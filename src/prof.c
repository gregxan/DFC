#include "prof.h"

prof_t g_prof[PROF_COUNT];

/* Names used on the diag status line; keep in step with prof_id_t. */
const char *const g_prof_name[PROF_COUNT] = {
    [PROF_EPOCH] = "epoch",
    [PROF_APP_MAG] = "mag",
    [PROF_APP_LINK0] = "link0",
    [PROF_APP_LINK1] = "link1",
};

uint32_t prof_avg(prof_id_t id) {
    const prof_t *p = &g_prof[id];
    return p->count != 0u ? (uint32_t)(p->sum / p->count) : 0u;
}
