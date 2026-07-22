#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Parse an iFit / NordicTrack / ProForm BLE treadmill data notification.
 *
 * Protocol (reverse-engineered by qdomyos-zwift, proformtreadmill.cpp):
 *   Service 00001533-1412-efde-1523-785feabcd123
 *   Notify  00001535-1412-efde-1523-785feabcd123
 *   Write   00001534-1412-efde-1523-785feabcd123
 * A data frame is 20 bytes:
 *   b[0..3] = 00 12 01 04   (frame type header)
 *   b[5]    = 0x2e          (T6.5S data-frame discriminator; ack frames differ)
 *   speed   = (b[11]<<8 | b[10]) / 100   km/h
 *   incline = (int16)(b[13]<<8 | b[12]) / 100   %   (signed)
 *   watts   = (b[15]<<8 | b[14])
 * The frame carries NO distance — callers integrate it from speed over time.
 *
 * Returns true and fills *speed_mps / *incline_pct for a valid data frame,
 * false otherwise (wrong length, wrong header, or not a data frame). */
bool ifit_parse_data(const uint8_t *buf, size_t len,
                     float *speed_mps, float *incline_pct);
