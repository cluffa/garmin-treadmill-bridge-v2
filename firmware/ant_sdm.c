#include "ant_sdm.h"
#include "ant_network_key.h"
#include "app_state.h"
#include "ant_sdm_encode.h"

#include "ant_interface.h"
#include "ant_parameters.h"
#include "app_error.h"
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
 * Cycle of 68 TX slots:
 *   0..63  page 1 (distance/speed)
 *   64..65 page 2 (cadence/status — zero cadence, treadmill has no stride)
 *   66     common page 80
 *   67     common page 81
 */
#define CYCLE_LEN     68
#define P2_FIRST      64
#define P80_SLOT      66
#define P81_SLOT      67

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
 * Distance is integrated from elapsed_s deltas on page-1 slots (page 1 goes
 * out ~3.76 Hz, so the delta is 0 or 1 s per encode), which stalls the
 * trace whenever the belt state stops updating (e.g. link loss).
 *
 * ⚠ That means this mode does NOT currently work with no treadmill connected,
 * which is one of the things it was wanted for. treadmill.elapsed_s is filled
 * in only by FTMS treadmill-data notifications (core/ftms_parse.c) and is
 * zeroed on disconnect (ble_central.c), so with no link the delta is always 0:
 * broadcast distance stays flat and the page-1 time field (elapsed_s % 256,
 * core/ant_sdm_encode.c) stays pinned at 0. Speed is still carried. To make
 * the standalone case work, integrate from a local monotonic tick (the
 * app_timer heartbeat) and synthesise the time field from it. */
static float s_tgt_dist_m   = 0.0f;   /* target-integrated distance, m */
static bool  s_tgt_seeded   = false;  /* seeded from actual distance on first page */
static uint8_t s_prev_elapsed = 0;    /* last elapsed_s seen on a page-1 slot */

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
        if (!s_tgt_seeded) {
            s_tgt_dist_m = ts_target.distance_m;   /* start where actual is */
            s_tgt_seeded = true;
        }
        if (s_slot < P2_FIRST) {
            uint8_t now = (uint8_t)ts_target.elapsed_s;
            uint8_t delta = (uint8_t)(now - s_prev_elapsed); /* wraps mod 256 */
            s_prev_elapsed = now;
            if (delta > 0 && delta <= 2) {   /* ~3.76 Hz pages: 0 or 1 s */
                s_tgt_dist_m += ts_target.speed_mps * (float)delta;
            }
        }
        ts_target.distance_m = s_tgt_dist_m;
        ts = &ts_target;
    } else {
        /* Re-seed from the actual distance next time target mode is enabled. */
        s_tgt_seeded = false;
    }

    if (s_slot == P80_SLOT) {
        memcpy(pg, PAGE_80, sizeof(pg));
    } else if (s_slot == P81_SLOT) {
        memcpy(pg, PAGE_81, sizeof(pg));
    } else if (s_slot >= P2_FIRST) {
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
