#include "ant_sdm.h"
#include "ant_network_key.h"
#include "app_state.h"
#include "ant_sdm_encode.h"
#include "ant_request.h"
#include "ctrl_dispatch.h"
#include "sdm_broadcast.h"

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
/* 8134 (~4.03 Hz) is what the ANT+ SDM device profile specifies, and it is not
 * a round number by accident: $SDK_ROOT/components/ant/ant_profiles/ant_sdm/
 * ant_sdm.h names SDM_MSG_PERIOD_4Hz = 8134 and SDM_MSG_PERIOD_2Hz = 16268 as
 * the *only* periods the profile supports. This was 8192 — a comment claiming
 * profile compliance over a value the profile does not allow — until
 * 2026-08-18, when the ANT sniffer (firmware/ant_log.c) caught a real treadmill
 * SDM sensor transmitting at a measured 248 ms median, i.e. 8134.
 *
 * A tracking slave re-syncs to the master it hears, so 8192 worked; this is
 * about not being 0.7% off a certified profile in a way nothing on the watch
 * side would ever report. See docs/ant-logging.md. */
#define SDM_CHANNEL_PERIOD 8134

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
 * Cycle of 64 TX slots (~15.9 s at 4.03 Hz), so each common page still repeats
 * every 64 messages, inside the 65-message requirement — which is counted in
 * MESSAGES, not seconds, so the channel period does not enter into it:
 *    7  page 22 (capabilities — cadence declared unsupported)
 *   15  page 2 (cadence/status — cadence invalid, treadmill has no stride)
 *   31  common page 80
 *   47  page 2
 *   63  common page 81
 *   all other slots: page 1 (distance/speed)
 *
 * Every background slot is ≡ 3 (mod 4), which is what preserves the interleave
 * invariant: any four consecutive slots — one second of air time, one watch
 * record — contain exactly one slot ≡ 3 (mod 4), so at most one of them is a
 * background page and at least three are page 1. That holds across the wrap
 * ({63,0,1,2}) too. Adding a background page at any other residue would put
 * two of them in some one-second window and reintroduce the speed holes.
 *
 * One thing does bend this, deliberately and briefly: answering a page 70
 * request (core/ant_request.c) displaces the rotation page for that slot, and
 * the rotation resumes rather than skipping ahead. Responses are spread one per
 * ANT_REQUEST_SPREAD slots and capped, so during a burst a one-second window
 * holds at most one response plus at most one rotation background page —
 * degrading the guarantee from three page 1s to two, never to zero. A request
 * arrives at pairing, not mid-interval, and two page 1s per window is still one
 * speed sample per record.
 */
#define CYCLE_LEN     64
#define P22_SLOT       7
#define P2_SLOT_A     15
#define P80_SLOT      31
#define P2_SLOT_B     47
#define P81_SLOT      63

/* Common page 80: HW revision 1, manufacturer 0x00FF (development), model 1.
 * Common page 81: SW revision 1, serial number 0xFFFFFFFF (none). */
static const uint8_t PAGE_80[8] = {0x50,0xFF,0xFF,0x01,0xFF,0x00,0x01,0x00};
static const uint8_t PAGE_81[8] = {0x51,0xFF,0xFF,0x01,0xFF,0xFF,0xFF,0xFF};

/* ---- Page 22: capabilities ---------------------------------------------------
 *
 * The whole point of this page is byte 1, and specifically the *cleared*
 * cadence bit. The bridge has no stride sensor and must not invent a cadence,
 * but the watch has been sourcing cadence from us anyway and recording a flat
 * zero for it — 2758 of 2890 records in 23856353712_ACTIVITY.fit, which then
 * poisons every metric Garmin derives from cadence (avg power 613 W, avg run
 * cadence 494 spm with max 0, step length 784 mm against a true 1139 mm).
 * The page-2 "invalid cadence" encoding (0xFF / 0xF) did not stop that, and
 * neither can anything else in the data pages: page 1's stride counter is a
 * rolling count with no invalid encoding, so a receiver differentiating it
 * always gets a well-formed zero. Page 22 is the only place in the profile
 * where a sensor can say the field does not exist. Declaring it unsupported
 * is what should push the watch back onto its own wrist cadence, which
 * §2 of docs/sdm-recording-analysis.md shows is correct and available.
 *
 * Byte 1 bit layout, verified against the SDK profile — this byte was the
 * blocker on shipping the page, because it also carries the speed and distance
 * validity bits and a wrong guess would flag the product's core function
 * unsupported, silently:
 *
 *   $SDK_ROOT/components/ant/ant_profiles/ant_sdm/pages/ant_sdm_page_22.h
 *     bit 0 time  bit 1 distance  bit 2 speed
 *     bit 3 latency  bit 4 cadence  bit 5 calories   (set = supported)
 *
 * so 0x07 = time + distance + speed supported; cadence, latency and calories
 * not. Latency stays clear because we never compute one — page 1 byte 7 is a
 * constant 0, and a receiver that honours this byte will ignore it.
 *
 * Bytes 2-7 are the page's reserved field: the same SDK header's encoder
 * (ant_sdm_page_22.c) memsets all six to UINT8_MAX.
 *
 * ⚠ Sent unsolicited on a slot in the rotation, AND on request. The SDK's own
 * SDM *master* only emits page 22 in reply to a page-70 request from the
 * display (next_page_number_get() takes it from the request controller and
 * never from the periodic rotation). The unsolicited broadcast came first
 * because the SDK *display* side decodes page 22 whenever it arrives, with no
 * request-matching, and because a broadcast reaches a watch that is already
 * paired while a request handler only helps if the watch actually asks.
 *
 * That was tried alone and did not work: 23943114012_ACTIVITY.fit, recorded
 * 2026-08-11 on a re-paired footpod with this page on air every 16 s, still
 * shows avg_running_cadence 252 and max 0 — and shows the watch using correct
 * wrist cadence for 41 s after a belt spin-up before reverting to our zero.
 * docs/sdm-recording-analysis.md §12. So the request path is now implemented
 * too (core/ant_request.c), which is the remaining explanation: a display that
 * learns capabilities only by asking. STATUS reports whether it ever asks. */
static const uint8_t PAGE_22[8] = {0x16,0x07,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

static uint8_t s_slot;    /* position in the CYCLE_LEN pattern */

/* Page-70 request state — see core/ant_request.h. */
static ant_request_t s_req;

/* Which page the rotation wants on this slot. */
static uint8_t rotation_page(uint8_t slot)
{
    if (slot == P80_SLOT)                        return 80;
    if (slot == P81_SLOT)                        return 81;
    if (slot == P22_SLOT)                        return 22;
    if (slot == P2_SLOT_A || slot == P2_SLOT_B)  return 2;
    return 1;
}

/* Build a page by number. Kept keyed on the number rather than the slot so the
 * rotation and a page-70 response go through the same encoder — a requested
 * page that disagreed with the broadcast one would be its own bug. */
static void build_page(uint8_t page_no, const treadmill_state_t *ts,
                       uint8_t pg[8])
{
    switch (page_no) {
        case 80: memcpy(pg, PAGE_80, 8);     break;
        case 81: memcpy(pg, PAGE_81, 8);     break;
        case 22: memcpy(pg, PAGE_22, 8);     break;
        case 2:  ant_sdm_encode_page2(ts, pg); break;
        default: ant_sdm_encode_page1(ts, pg); break;
    }
}

/* ---- The footpod's own clock -------------------------------------------------
 *
 * A footpod times itself. This clock is integrated from the app_timer RTC
 * (32768 Hz) on every TX event and is what page 1's time field carries, in
 * *both* broadcast modes.
 *
 * It used to come from treadmill.elapsed_s, which is not a footpod clock at
 * all: ble_central.c's iFit path assigns it a literal 0 on every notification
 * (iFit frames carry no elapsed time), and core/ftms_parse.c fills it only
 * when the treadmill sets Treadmill Data flag bit 10, memset-ing it to 0
 * otherwise. So on an iFit treadmill the footpod broadcast a frozen clock for
 * the whole run while its distance field advanced normally — a sensor whose
 * odometer moves against a stopped clock. Observed in
 * 23856353712_ACTIVITY.fit (2026-08-05): every lap recorded total_distance 0
 * and no avg_speed at all, and the session cadence/running-dynamics
 * accumulators came out as garbage (avg_running_cadence 246 strides/min with
 * max_running_cadence 0). See docs/sdm-recording-analysis.md.
 *
 * Wrapped at 256 s because that is where the wire field rolls anyway (the
 * receiver reassembles it), and holding the accumulator small keeps float32
 * resolution constant instead of degrading over a multi-hour session.
 *
 * Integrating on every TX event (~4 Hz) rather than per page-1 slot keeps the
 * trace smooth, and the RTC keeps running when the belt state goes quiet. */
#define SDM_TIME_ROLL_S  256.0f

static float    s_elapsed_s  = 0.0f;   /* footpod clock, s, wrapped at 256 */
static uint32_t s_prev_tick  = 0;      /* app_timer ticks at the last encode */
static bool     s_clock_run  = false;  /* s_prev_tick is valid */

/* Broadcast speed/distance policy — see core/sdm_broadcast.h. */
static sdm_broadcast_t s_bcast;

/* ---- Target-broadcast debug mode --------------------------------------------
 *
 * When app_state()->sdm_broadcast_target is set (testboard button action
 * SDM:TGT), the footpod broadcasts the bridge's resolved target speed
 * (resolved_target_mps) instead of the actual belt speed, and distance is
 * integrated from that target. The watch then records exactly what the
 * bridge commanded: run the same workout in normal mode for the actual
 * belt trace and diff the two .fit files to score belt accuracy.
 *
 * Only the *distance* is special-cased here now — the clock above is shared,
 * so this mode works with no treadmill connected for the same reason it
 * always did. The target speed itself latches with no treadmill:
 * machine_set_speed() writes resolved_target_mps unconditionally, before its
 * connection check. */
static float    s_tgt_dist_m    = 0.0f;   /* target-integrated distance, m */
static bool     s_tgt_seeded    = false;  /* seeded from actual state on entry */

/* ---- ANT event observer callback ------------------------------------------- */
static void ant_evt_handler(ant_evt_t *p_evt, void *p_context)
{
    (void)p_context;
    if (p_evt->channel != SDM_CHANNEL) {
        return;
    }

    switch (p_evt->event) {
    case EVENT_RX:
        /* A master channel receives whatever the display sends it. The only
         * thing we act on is a page 70 request; ant_request_on_rx() ignores
         * everything else without counting it. */
        if (ant_request_on_rx(&s_req, p_evt->message.ANT_MESSAGE_aucPayload,
                              ANT_STANDARD_DATA_PAYLOAD_SIZE)) {
            NRF_LOG_INFO("ant_sdm page 70: page %u requested",
                         s_req.last_page);
        }
        break;

    /* Every one of these is one TX slot elapsing, and exactly one of them
     * arrives per slot. Pumping on all three is not optional: an acknowledged
     * response completes as EVENT_TRANSFER_TX_COMPLETED or _FAILED *instead of*
     * EVENT_TX, so a handler that only watched EVENT_TX would stop feeding the
     * channel the moment it answered its first request, and the radio would sit
     * there retransmitting one stale page forever. The SDK's own
     * ant_sdm_sens_evt_handler() takes all three for the same reason. */
    case EVENT_TRANSFER_TX_COMPLETED:
        ant_request_on_response_ack(&s_req, true);
        ant_sdm_on_tx_event();
        break;

    case EVENT_TRANSFER_TX_FAILED:
        ant_request_on_response_ack(&s_req, false);
        ant_sdm_on_tx_event();
        break;

    case EVENT_TX:
        ant_sdm_on_tx_event();
        break;

    default:
        break;
    }
}

NRF_SDH_ANT_OBSERVER(m_sdm_observer, APP_ANT_OBSERVER_PRIO, ant_evt_handler, NULL);

/* ---- Public API ------------------------------------------------------------ */

void ant_sdm_on_tx_event(void)
{
    uint8_t pg[8];
    app_state_t *st = app_state();

    /* Advance the footpod clock first — it runs in both modes, and with no
     * treadmill connected, because that is what a footpod does.
     *
     * app_timer_cnt_diff_compute() handles the RTC's 24-bit wrap (every
     * ~512 s at 32768 Hz), which a plain subtraction would not. The first
     * event after start has no predecessor to difference against, so it
     * contributes no time rather than a garbage delta. */
    uint32_t now_tick = app_timer_cnt_get();
    float dt = 0.0f;
    if (s_clock_run) {
        uint32_t d_ticks = app_timer_cnt_diff_compute(now_tick, s_prev_tick);
        /* Effective tick rate, matching the SDK's own APP_TIMER_TICKS(): the
         * raw RTC clock divided by the configured prescaler. Spelling it out
         * keeps this correct if APP_TIMER_CONFIG_RTC_FREQUENCY ever moves off
         * 0 (app_config.h), which a bare APP_TIMER_CLOCK_FREQ would not. */
        dt = (float)d_ticks /
             ((float)APP_TIMER_CLOCK_FREQ /
              (float)(APP_TIMER_CONFIG_RTC_FREQUENCY + 1));
    }
    s_prev_tick = now_tick;
    s_clock_run = true;

    s_elapsed_s += dt;
    while (s_elapsed_s >= SDM_TIME_ROLL_S) {
        s_elapsed_s -= SDM_TIME_ROLL_S;
    }

    /* Every broadcast carries the footpod's clock, never the treadmill's. */
    treadmill_state_t ts_bcast = st->treadmill;
    ts_bcast.elapsed_s = s_elapsed_s;

    /* ...and likewise its own speed and odometer. ble_central.c zeroes the
     * treadmill struct on disconnect, which broadcast a dead stop through
     * every BLE dropout while the belt kept running; the policy in
     * core/sdm_broadcast.c holds speed across a brief drop and keeps the
     * odometer monotonic across the source's resets. A stop the treadmill
     * actually reports still passes straight through.
     *
     * Known and accepted: central_link goes LINK_UP a little before the first
     * notification repopulates speed, so a tick or two of the post-memset zero
     * can reach the air on reconnect — sub-second, and below the watch's 1 Hz
     * record rate. */
    sdm_broadcast_step(&s_bcast, st->central_link == LINK_UP,
                       st->treadmill.speed_mps, st->treadmill.distance_m, dt,
                       &ts_bcast.speed_mps, &ts_bcast.distance_m);

    /* Target-broadcast debug mode: override the belt state with the
     * commanded target. Applies to every page (1 and 2 both carry speed). */
    if (st->sdm_broadcast_target) {
        ts_bcast.speed_mps = st->resolved_target_mps;

        if (!s_tgt_seeded) {
            /* Start where the actual belt is so toggling mid-run does not
             * jump the trace; with no treadmill this is 0. */
            s_tgt_dist_m = ts_bcast.distance_m;
            s_tgt_seeded = true;
        }
        s_tgt_dist_m += ts_bcast.speed_mps * dt;
        ts_bcast.distance_m = s_tgt_dist_m;
    } else {
        /* Re-seed from the actual belt state next time target mode is
         * enabled, so the trace picks up from where the belt actually is. */
        s_tgt_seeded = false;
    }

    const treadmill_state_t *ts = &ts_bcast;

    /* A page-70 response takes the slot from the rotation, which then resumes
     * where it left off rather than skipping the page it was owed — the
     * background pages stay in their pattern, just one slot later. */
    uint8_t page_no;
    bool    ack        = false;
    bool    responding = ant_request_take_slot(&s_req, &page_no, &ack);
    if (!responding) {
        page_no = rotation_page(s_slot);
        s_slot  = (uint8_t)((s_slot + 1) % CYCLE_LEN);
    }

    build_page(page_no, ts, pg);

    uint32_t err = ack
        ? sd_ant_acknowledge_message_tx(SDM_CHANNEL, sizeof(pg), pg)
        : sd_ant_broadcast_message_tx(SDM_CHANNEL, sizeof(pg), pg);
    if (err != NRF_SUCCESS) {
        NRF_LOG_WARNING("ant_sdm tx err %u", err);
        /* Nothing reached the air, so a refused response is still owed. Never
         * APP_ERROR_CHECK here: a footpod that resets because one page did not
         * go out is worse than one that misses a page. */
        if (responding) {
            ant_request_retry_slot(&s_req);
        }
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

    /* Restart the footpod clock with this broadcast session. s_clock_run
     * clear makes the priming encode below contribute no dt, so the first
     * page goes out at t = 0 rather than at however long the board had been
     * powered. */
    s_elapsed_s = 0.0f;
    s_clock_run = false;
    sdm_broadcast_init(&s_bcast);

    /* Called once, from main(), so the page-70 counters STATUS reports are
     * per-boot and not silently reset by a treadmill reconnect. */
    ant_request_init(&s_req);

    /* Prime and send the first broadcast page, then open the channel.
     * Subsequent pages go out from EVENT_TX. */
    ant_sdm_on_tx_event();
    APP_ERROR_CHECK(sd_ant_channel_open(SDM_CHANNEL));

    app_state()->ant_broadcasting = true;
    NRF_LOG_INFO("ant_sdm broadcasting");
}

/* ---- STATUS readout --------------------------------------------------------
 *
 * Strong overrides of core's weak page-70 accessors, so the console can answer
 * "does the watch ever ask us for a page?" — the one question that separates the
 * two surviving explanations for the zero-cadence recordings, and the one a
 * .FIT cannot answer. See core/ctrl_dispatch.h and
 * docs/sdm-recording-analysis.md §12. */
uint32_t ctrl_sdm_p70_requests(void)  { return s_req.requests; }
uint32_t ctrl_sdm_p70_rejects(void)   { return s_req.rejects; }
uint8_t  ctrl_sdm_p70_last_page(void) { return s_req.last_page; }
