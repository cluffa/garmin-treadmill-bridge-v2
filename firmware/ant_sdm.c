#include "ant_sdm.h"
#include "ant_network_key.h"
#include "app_state.h"
#include "ant_sdm_encode.h"

#include "ant_interface.h"
#include "ant_parameters.h"
#include "app_error.h"
#include "app_timer.h"
#include "nrf.h"
#include "nrf_log.h"
#include "nrf_sdh_ant.h"

#include <string.h>

/* ---- ANT+ SDM channel configuration ---------------------------------------- */
#define SDM_CHANNEL        0
#define SDM_NETWORK        0
#define SDM_DEVICE_TYPE    124     /* ANT+ SDM (Stride-Based Speed & Distance) */
#define SDM_TRANS_TYPE     0x05    /* independent channel, per SDM profile    */
#define SDM_RF_FREQ        57      /* 2457 MHz — ANT+                         */
#define SDM_CHANNEL_PERIOD 8192    /* ~4 Hz, per the SDM device profile       */

/* ---- Page cadence (SDM profile) --------------------------------------------
 *
 * Main data page with periodic background pages. Common pages 80/81
 * (manufacturer/product info) are required at least once every 65 messages
 * for the receiver to identify the sensor.
 *
 * The four background slots are *spread*, never adjacent. They used to sit in
 * one run at the end of a 68-slot cycle (p2, p2, p80, p81), which at ~4 Hz is a
 * full second in which the watch receives no page 1 at all — and the watch
 * records one speed sample per second. That lines up with the ~1 s speed
 * holes test/pace_lag_report.py found in the 2026-08-01 trace (distance kept
 * advancing through them, so nothing had actually stopped). Interleaving
 * guarantees at least three page 1s inside any one-second window.
 *
 * Cycle of 64 TX slots (~16 s at 4 Hz), so each common page still repeats
 * every 64 messages, inside the 65-message requirement:
 *   15  page 2 (cadence/status — cadence invalid, treadmill has no stride)
 *   31  common page 80
 *   47  page 2
 *   63  common page 81
 *   all other slots: page 1 (distance/speed)
 */
#define CYCLE_LEN     64
#define P2_SLOT_A     15
#define P80_SLOT      31
#define P2_SLOT_B     47
#define P81_SLOT      63

/* Common page 80: HW revision 1, manufacturer 0x00FF (development), model 1.
 * Common page 81: SW revision 1, serial number 0xFFFFFFFF (none). */
static const uint8_t PAGE_80[8] = {0x50,0xFF,0xFF,0x01,0xFF,0x00,0x01,0x00};
static const uint8_t PAGE_81[8] = {0x51,0xFF,0xFF,0x01,0xFF,0xFF,0xFF,0xFF};

static uint8_t s_slot;    /* position in the CYCLE_LEN pattern */

/* ---- Target-broadcast debug mode --------------------------------------------
 *
 * When app_state()->sdm_broadcast_target is set (testboard button action
 * SDM:TGT), the footpod broadcasts the bridge's resolved target speed
 * (resolved_target_mps) instead of the actual belt speed, and distance is
 * integrated from that target. The watch then records exactly what the
 * bridge commanded: run the same workout in normal mode for the actual
 * belt trace and diff the two .fit files to score belt accuracy.
 *
 * Both the elapsed time and the distance this mode broadcasts are integrated
 * from the app_timer RTC (32768 Hz), NOT from treadmill.elapsed_s — that field
 * is filled in only by FTMS treadmill-data notifications (core/ftms_parse.c)
 * and is zeroed on disconnect (ble_central.c), so keying off it pinned both
 * the time and distance fields at 0 whenever no treadmill was connected, which
 * is precisely the case this mode exists to serve. The target speed itself
 * does latch with no treadmill: machine_set_speed() writes
 * resolved_target_mps unconditionally, before its connection check.
 *
 * Integrating on every TX event (~4 Hz) rather than per page-1 slot also keeps
 * the trace smooth, and the RTC keeps running when the belt state goes quiet. */
static float    s_tgt_dist_m    = 0.0f;   /* target-integrated distance, m */
static float    s_tgt_elapsed_s = 0.0f;   /* target-integrated elapsed time, s */
static bool     s_tgt_seeded    = false;  /* seeded from actual state on entry */
static uint32_t s_tgt_prev_tick = 0;      /* app_timer ticks at the last encode */

/* ---- ANT event observer callback ------------------------------------------- */
static void ant_evt_handler(ant_evt_t *p_evt, void *p_context)
{
    (void)p_context;
    if (p_evt->channel != SDM_CHANNEL) {
        return;
    }
    if (p_evt->event == EVENT_TX) {
        ant_sdm_on_tx_event();
    }
}

NRF_SDH_ANT_OBSERVER(m_sdm_observer, APP_ANT_OBSERVER_PRIO, ant_evt_handler, NULL);

/* ---- Public API ------------------------------------------------------------ */

void ant_sdm_on_tx_event(void)
{
    uint8_t pg[8];
    app_state_t *st = app_state();

    /* Target-broadcast debug mode: override the belt state with the
     * commanded target. Applies to every page (1 and 2 both carry speed). */
    treadmill_state_t ts_target;
    const treadmill_state_t *ts = &st->treadmill;
    if (st->sdm_broadcast_target) {
        ts_target = *ts;
        ts_target.speed_mps = st->resolved_target_mps;

        uint32_t now_tick = app_timer_cnt_get();
        if (!s_tgt_seeded) {
            /* Start where the actual belt is so toggling mid-run does not
             * jump the trace; with no treadmill both of these are 0. */
            s_tgt_dist_m    = ts_target.distance_m;
            s_tgt_elapsed_s = (float)ts_target.elapsed_s;
            s_tgt_prev_tick = now_tick;
            s_tgt_seeded    = true;
        }

        /* app_timer_cnt_diff_compute() handles the RTC's 24-bit wrap (every
         * ~512 s at 32768 Hz), which a plain subtraction would not. */
        uint32_t d_ticks = app_timer_cnt_diff_compute(now_tick, s_tgt_prev_tick);
        s_tgt_prev_tick  = now_tick;

        /* Effective tick rate, matching the SDK's own APP_TIMER_TICKS(): the
         * raw RTC clock divided by the configured prescaler. Spelling it out
         * keeps this correct if APP_TIMER_CONFIG_RTC_FREQUENCY ever moves off
         * 0 (app_config.h), which a bare APP_TIMER_CLOCK_FREQ would not. */
        float dt = (float)d_ticks /
                   ((float)APP_TIMER_CLOCK_FREQ /
                    (float)(APP_TIMER_CONFIG_RTC_FREQUENCY + 1));
        s_tgt_elapsed_s += dt;
        s_tgt_dist_m    += ts_target.speed_mps * dt;

        ts_target.elapsed_s  = (uint32_t)s_tgt_elapsed_s;
        ts_target.distance_m = s_tgt_dist_m;
        ts = &ts_target;
    } else {
        /* Re-seed from the actual belt state next time target mode is
         * enabled, so the trace picks up from where the belt actually is. */
        s_tgt_seeded = false;
    }

    if (s_slot == P80_SLOT) {
        memcpy(pg, PAGE_80, sizeof(pg));
    } else if (s_slot == P81_SLOT) {
        memcpy(pg, PAGE_81, sizeof(pg));
    } else if (s_slot == P2_SLOT_A || s_slot == P2_SLOT_B) {
        ant_sdm_encode_page2(ts, pg);
    } else {
        ant_sdm_encode_page1(ts, pg);
    }
    s_slot = (uint8_t)((s_slot + 1) % CYCLE_LEN);

    uint32_t err = sd_ant_broadcast_message_tx(SDM_CHANNEL, sizeof(pg), pg);
    if (err != NRF_SUCCESS) {
        NRF_LOG_WARNING("ant_sdm tx err %u", err);
    }
}

void ant_sdm_init(void)
{
    /* Device number from the chip id — stable across boots, unique enough.
     * ANT wildcard (0) is never valid as a device number. */
    uint16_t dev_num = (uint16_t)(NRF_FICR->DEVICEID[0] & 0xFFFF);
    if (dev_num == 0) {
        dev_num = 1;
    }

    APP_ERROR_CHECK(sd_ant_network_address_set(SDM_NETWORK,
                                               ANT_PLUS_NETWORK_KEY));
    APP_ERROR_CHECK(sd_ant_channel_assign(SDM_CHANNEL,
                                          CHANNEL_TYPE_MASTER,
                                          SDM_NETWORK, 0));
    APP_ERROR_CHECK(sd_ant_channel_id_set(SDM_CHANNEL, dev_num,
                                          SDM_DEVICE_TYPE, SDM_TRANS_TYPE));
    APP_ERROR_CHECK(sd_ant_channel_radio_freq_set(SDM_CHANNEL, SDM_RF_FREQ));
    APP_ERROR_CHECK(sd_ant_channel_period_set(SDM_CHANNEL, SDM_CHANNEL_PERIOD));

    NRF_LOG_INFO("ant_sdm init, dev_num=%u", dev_num);
}

void ant_sdm_start(void)
{
    s_slot = 0;

    /* Prime and send the first broadcast page, then open the channel.
     * Subsequent pages go out from EVENT_TX. */
    ant_sdm_on_tx_event();
    APP_ERROR_CHECK(sd_ant_channel_open(SDM_CHANNEL));

    app_state()->ant_broadcasting = true;
    NRF_LOG_INFO("ant_sdm broadcasting");
}
