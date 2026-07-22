#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "model.h"

bool ftms_parse_treadmill_data(const uint8_t *buf, size_t len,
                               treadmill_state_t *out);
