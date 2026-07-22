#pragma once
#include <stdint.h>
#include "model.h"

/* Encode ANT+ Stride-Based Speed & Distance Monitor (SDM) broadcast pages
 * from a treadmill state. Each page is exactly 8 bytes. Pure/stateless.
 *
 * Page 1 (0x01): time/distance/speed — the main data page.
 * Page 2 (0x02): cadence/status — required background page; we send zero
 * cadence (a treadmill has no stride sensor) and the same speed nibbles.
 *
 * Layouts follow the ANT+ SDM Device Profile; distance fraction is 1/16 m,
 * speed is a 4-bit integer (m/s) + 1/256 m/s fraction. Rolling 8-bit
 * time/distance counters wrap per the profile (receiver accumulates). */
void ant_sdm_encode_page1(const treadmill_state_t *s, uint8_t out[8]);
void ant_sdm_encode_page2(const treadmill_state_t *s, uint8_t out[8]);
