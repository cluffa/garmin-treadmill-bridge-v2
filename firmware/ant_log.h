#pragma once
/*
 * ant_log.h — ANT+ receive/sniff channel, for comparing another sensor's
 * broadcast against the footpod this bridge transmits.
 *
 * The bridge is an ANT *master* (firmware/ant_sdm.c, channel 0). This is a
 * second, independent *slave* channel (channel 1) that locks onto some other
 * ANT+ sensor in the room and dumps every page it receives to the USB console,
 * one line per message. Nothing in the product path reads it; it exists so a
 * real footpod / treadmill sensor and our synthesized one can be logged side by
 * side from the same radio and the same clock.
 *
 * Driven from the console, never from boot — see core/ctrl_dispatch.h
 * (`ANTLOG`). Off by default: an open slave channel costs radio time that
 * belongs to the footpod broadcast the watch is recording.
 *
 * Output lines (USB-CDC, one per received message):
 *   AL <seq> <ms> <devnum> <devtype> <trans> <16 hex chars>
 * plus state lines:
 *   {"event":"antlog","state":"..."}
 */

#include <stdbool.h>
#include <stdint.h>

/* Open (or re-open) the sniffer channel. Wildcards: dev_num 0, dev_type 0 and
 * trans_type 0 each mean "any". `period` is in 32768 Hz counts (0 = pick from
 * dev_type: 8134 SDM, 8070 HRM, else 8192) and `freq` is the ANT RF frequency
 * (0 = 57, i.e. 2457 MHz, ANT+).
 *
 * Returns NRF_SUCCESS or the first SoftDevice error; on error the channel is
 * left closed. Never asserts: a mistyped console command must not reset a
 * board that is mid-workout. */
uint32_t ant_log_start(uint16_t dev_num, uint8_t dev_type, uint8_t trans_type,
                       uint16_t period, uint8_t freq);

/* Close and unassign the channel. Safe to call when not running. */
uint32_t ant_log_stop(void);

/* JSON status line into `buf` — running, configured id, message/dropout
 * counters, and the id actually locked onto (which is what a wildcard search
 * found). */
void ant_log_status(char *buf, uint32_t cap);
