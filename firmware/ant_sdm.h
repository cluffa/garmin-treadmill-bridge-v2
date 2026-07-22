#pragma once
/*
 * ant_sdm.h — ANT+ SDM (Stride-Based Speed & Distance Monitor) master
 * footpod broadcast. Sends treadmill speed/distance/cadence as an
 * ANT+ SDM sensor so Garmin watches can read pace natively.
 *
 * Public API:
 *   ant_sdm_init()        — configure channel, assign ID, set network key
 *   ant_sdm_start()       — open channel + begin broadcasting
 *   ant_sdm_on_tx_event() — handle TX event: encode + broadcast next page
 */

void ant_sdm_init(void);
void ant_sdm_start(void);
void ant_sdm_on_tx_event(void);
