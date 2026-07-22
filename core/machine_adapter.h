#pragma once
#include "model.h"

typedef void (*machine_state_cb)(const treadmill_state_t *s);

/* Adapter -> facade event hook: connected != 0 on a successful connection,
 * 0 on disconnect. Lets the facade own reconnect/persistence policy. */
typedef void (*machine_event_cb)(int connected);
