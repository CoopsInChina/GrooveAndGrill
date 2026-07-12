#include "bbq_controller.h"
#include <string.h>

static bbq_grill_t s_grills[MAX_GRILLS];
static int         s_count = 0;

void bbq_controller_init(void)
{
    memset(s_grills, 0, sizeof(s_grills));
    s_count = 1;   // must always have at least 1 grill
}

int bbq_grill_count(void)
{
    return s_count;
}

bool bbq_add_grill(void)
{
    if (s_count >= MAX_GRILLS) return false;
    s_grills[s_count] = (bbq_grill_t){0};
    s_count++;
    return true;
}

const bbq_grill_t *bbq_get_grill(int idx)
{
    if (idx < 0 || idx >= s_count) return NULL;
    return &s_grills[idx];
}

void bbq_mock_connect_probe(int idx)
{
    if (idx < 0 || idx >= s_count) return;
    s_grills[idx].probe_state = PROBE_CONNECTED;
    s_grills[idx].meat_temp_c  = 25.0f;
    s_grills[idx].grill_temp_c = 350.0f;
}

void bbq_mock_toggle_probe(int idx)
{
    if (idx < 0 || idx >= s_count) return;
    bbq_grill_t *g = &s_grills[idx];
    if (g->probe_state == PROBE_CONNECTED) {
        g->probe_state = PROBE_DISCONNECTED;
    } else if (g->probe_state == PROBE_DISCONNECTED) {
        g->probe_state = PROBE_CONNECTED;
        g->meat_temp_c  = 25.0f;
        g->grill_temp_c = 350.0f;
    }
}

void bbq_set_targets(int idx, int grill_target_c, int meat_target_c, meat_kind_t kind)
{
    if (idx < 0 || idx >= s_count) return;
    s_grills[idx].configured      = true;
    s_grills[idx].meat_kind        = kind;
    s_grills[idx].grill_target_c  = grill_target_c;
    s_grills[idx].meat_target_c   = meat_target_c;
}

void bbq_set_grill_target(int idx, int grill_target_c)
{
    if (idx < 0 || idx >= s_count) return;
    s_grills[idx].grill_target_c = grill_target_c;
}
