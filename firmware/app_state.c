/*
 * app_state.c — singleton implementation.
 *
 * M2 Task 2.1.
 */

#include "app_state.h"
#include <string.h>

static app_state_t s_state;

app_state_t *app_state(void)
{
    return &s_state;
}

void app_state_init(void)
{
    memset(&s_state, 0, sizeof(s_state));
}

void app_state_set_fault(uint32_t code)
{
    s_state.last_fault_code = code;
}
