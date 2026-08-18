#include "ant_log.h"
#include "ant_network_key.h"
#include "ctrl_dispatch.h"
#include "usb_cdc_log.h"

#include "ant_interface.h"
#include "ant_parameters.h"
#include "app_timer.h"
#include "nrf_log.h"
#include "nrf_sdh_ant.h"

#include <stdio.h>
#include <string.h>

/* Channel 1. Channel 0 is the footpod master (ant_sdm.c) and must keep it:
 * NRF_SDH_ANT_TOTAL_CHANNELS_ALLOCATED is 2 in app_config.h for exactly this. */
#define ANTLOG_CHANNEL   1
#define ANTLOG_NETWORK   0
#define ANTLOG_RF_FREQ   57      /* 2457 MHz — ANT+ */

/* Per-profile message periods, in 32768 Hz counts, from the SDK's own profile
 * headers ($SDK_ROOT/components/ant/ant_profiles). They differ, and a slave
 * whose period disagrees with the master it locked onto tracks for a moment and
 * then drops messages, so the default is chosen from the device type rather
 * than being one constant.
 *
 * This tool is what found the SDM row: our own master transmitted at 8192 under
 * a comment claiming profile compliance until 2026-08-18, when a real sensor
 * logged here came in at a measured 248 ms. ant_sdm.c is 8134 now, so the two
 * agree — keep them that way. */
#define PERIOD_SDM       8134u   /* device type 124 */
#define PERIOD_HRM       8070u   /* device type 120 */
#define PERIOD_DEFAULT   8192u   /* FE-C (17), and anything unrecognised */

static bool     s_running;
static bool     s_assigned;
static bool     s_locked;        /* have received at least one message */
static bool     s_pending;       /* a start is waiting for a close to finish */

static uint16_t s_cfg_num;
static uint8_t  s_cfg_type;
static uint8_t  s_cfg_trans;
static uint16_t s_cfg_period;
static uint8_t  s_cfg_freq;

static uint16_t s_lock_num;
static uint8_t  s_lock_type;
static uint8_t  s_lock_trans;

static uint32_t s_msgs;          /* messages received */
static uint32_t s_rx_fails;      /* missed receive slots */
static uint32_t s_searches;      /* times the channel fell back to searching */

/* Message clock. Counted in RTC ticks and converted only when a line is
 * printed, so the timestamps stay exact against the same app_timer the footpod
 * clock in ant_sdm.c is integrated from — the point of logging both sensors
 * from this board is that they share one time base. */
static uint32_t s_ticks;
static uint32_t s_prev_tick;
static bool     s_clock_run;

static uint32_t apply_config(void);

static uint32_t tick_hz(void)
{
    return (uint32_t)APP_TIMER_CLOCK_FREQ /
           (uint32_t)(APP_TIMER_CONFIG_RTC_FREQUENCY + 1);
}

static uint32_t elapsed_ms(void)
{
    uint32_t now = app_timer_cnt_get();
    if (s_clock_run) {
        s_ticks += app_timer_cnt_diff_compute(now, s_prev_tick);
    }
    s_prev_tick = now;
    s_clock_run = true;
    return (uint32_t)(((uint64_t)s_ticks * 1000u) / tick_hz());
}

static uint16_t default_period(uint8_t dev_type)
{
    switch (dev_type) {
        case 124: return PERIOD_SDM;
        case 120: return PERIOD_HRM;
        default:  return PERIOD_DEFAULT;
    }
}

static void emit_state(const char *state)
{
    char buf[96];
    snprintf(buf, sizeof buf,
             "{\"event\":\"antlog\",\"state\":\"%s\",\"dev\":%u,\"type\":%u,"
             "\"msgs\":%lu}",
             state, (unsigned)s_lock_num, (unsigned)s_lock_type,
             (unsigned long)s_msgs);
    usb_cdc_log_write(buf);
}

/* ---- ANT event observer ----------------------------------------------------
 *
 * A second observer, filtered to this channel. ant_sdm.c's observer filters to
 * channel 0, so the two never see each other's events. */
static void ant_log_evt_handler(ant_evt_t *p_evt, void *p_context)
{
    (void)p_context;
    if (p_evt->channel != ANTLOG_CHANNEL) {
        return;
    }

    switch (p_evt->event) {
    case EVENT_RX: {
        uint32_t ms = elapsed_ms();
        s_msgs++;

        /* Read the id back on every message rather than caching it once. With
         * a wildcard search the channel adopts whatever master it found, and
         * after a dropout it can adopt a *different* one — a cached id would
         * then label another sensor's pages with the first sensor's number,
         * which is the one mistake that would quietly invalidate the whole
         * comparison this tool exists for. */
        (void)sd_ant_channel_id_get(ANTLOG_CHANNEL, &s_lock_num, &s_lock_type,
                                    &s_lock_trans);

        if (!s_locked) {
            s_locked = true;
            emit_state("locked");
        }

        const uint8_t *p = p_evt->message.ANT_MESSAGE_aucPayload;
        char buf[80];
        /* Sequence number is the message counter, so a gap in the log is a
         * dropout and not a lost console line — the two look identical
         * otherwise, and telling them apart is the comparison. */
        snprintf(buf, sizeof buf,
                 "AL %lu %lu %u %u %u %02X%02X%02X%02X%02X%02X%02X%02X",
                 (unsigned long)s_msgs, (unsigned long)ms,
                 (unsigned)s_lock_num, (unsigned)s_lock_type,
                 (unsigned)s_lock_trans,
                 p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
        usb_cdc_log_write(buf);
        break;
    }

    /* One missed receive slot. Counted, never printed: out of range this fires
     * at the channel period (~4 Hz) and would bury the data lines in the very
     * log you are reading to find dropouts. */
    case EVENT_RX_FAIL:
        s_rx_fails++;
        break;

    case EVENT_RX_FAIL_GO_TO_SEARCH:
        s_searches++;
        s_locked = false;
        emit_state("searching");
        break;

    case EVENT_RX_SEARCH_TIMEOUT:
        emit_state("search_timeout");
        break;

    case EVENT_CHANNEL_CLOSED:
        /* The channel is only genuinely down here, which makes this the one
         * safe place to unassign it — and therefore the place a deferred
         * re-open has to happen. */
        s_running = false;
        if (s_assigned) {
            (void)sd_ant_channel_unassign(ANTLOG_CHANNEL);
            s_assigned = false;
        }
        if (s_pending) {
            s_pending = false;
            uint32_t err = apply_config();
            if (err != NRF_SUCCESS) {
                char buf[64];
                snprintf(buf, sizeof buf,
                         "{\"event\":\"antlog\",\"state\":\"start_failed\","
                         "\"err\":%lu}", (unsigned long)err);
                usb_cdc_log_write(buf);
            } else {
                emit_state("reopened");
            }
        } else {
            emit_state("closed");
        }
        break;

    default:
        break;
    }
}

NRF_SDH_ANT_OBSERVER(m_antlog_observer, APP_ANT_OBSERVER_PRIO,
                     ant_log_evt_handler, NULL);

/* ---- Public API ------------------------------------------------------------ */

/* Assign, configure and open the channel from s_cfg_*. Split out of
 * ant_log_start() because it also has to run later, from the close event, when
 * a re-open had to wait for the previous channel to actually go down. */
static uint32_t apply_config(void)
{
    uint32_t err;

    /* Network 0's key is already the ANT+ key — ant_sdm_init() set it at boot
     * and the two channels share the network. Setting it again here would be
     * harmless but would also mean this file has an opinion about a global the
     * footpod owns. */
    err = sd_ant_channel_assign(ANTLOG_CHANNEL, CHANNEL_TYPE_SLAVE_RX_ONLY,
                                ANTLOG_NETWORK, 0);
    if (err != NRF_SUCCESS) return err;
    s_assigned = true;

    err = sd_ant_channel_id_set(ANTLOG_CHANNEL, s_cfg_num, s_cfg_type,
                                s_cfg_trans);
    if (err != NRF_SUCCESS) goto fail;
    err = sd_ant_channel_radio_freq_set(ANTLOG_CHANNEL, s_cfg_freq);
    if (err != NRF_SUCCESS) goto fail;
    err = sd_ant_channel_period_set(ANTLOG_CHANNEL, s_cfg_period);
    if (err != NRF_SUCCESS) goto fail;

    /* Search priority is the whole reason this is safe to run during a workout.
     * High-priority search blocks other channels' timeslots outright, and the
     * channel it would block is the footpod broadcast the watch is recording —
     * the reference side of the comparison. 0 disables it, so the search runs
     * at low priority and yields to the master.
     *
     * Low-priority timeout 255 is infinite. Anything finite closes the channel
     * silently after that many 2.5 s units of not hearing the sensor, and a
     * footpod that has not been shaken yet is asleep: the default (~25 s) would
     * expire before the user got on the treadmill, and "no lines" would read as
     * a broken tool. */
    err = sd_ant_channel_search_timeout_set(ANTLOG_CHANNEL, 0);
    if (err != NRF_SUCCESS) goto fail;
    err = sd_ant_channel_low_priority_rx_search_timeout_set(ANTLOG_CHANNEL, 255);
    if (err != NRF_SUCCESS) goto fail;

    err = sd_ant_channel_open(ANTLOG_CHANNEL);
    if (err != NRF_SUCCESS) goto fail;

    s_running   = true;
    s_locked    = false;
    s_msgs      = 0;
    s_rx_fails  = 0;
    s_searches  = 0;
    s_ticks     = 0;
    s_clock_run = false;
    s_lock_num   = s_cfg_num;
    s_lock_type  = s_cfg_type;
    s_lock_trans = s_cfg_trans;

    NRF_LOG_INFO("ant_log: open dev=%u type=%u period=%u",
                 s_cfg_num, s_cfg_type, s_cfg_period);
    return NRF_SUCCESS;

fail:
    (void)sd_ant_channel_unassign(ANTLOG_CHANNEL);
    s_assigned = false;
    s_running  = false;
    return err;
}

uint32_t ant_log_start(uint16_t dev_num, uint8_t dev_type, uint8_t trans_type,
                       uint16_t period, uint8_t freq)
{
    if (period == 0) period = default_period(dev_type);
    if (freq   == 0) freq   = ANTLOG_RF_FREQ;

    s_cfg_num    = dev_num;
    s_cfg_type   = dev_type;
    s_cfg_trans  = trans_type;
    s_cfg_period = period;
    s_cfg_freq   = freq;

    /* Re-opening with new parameters is the normal way to use this: you start
     * wildcard, see what locked, then pin it. But closing an ANT channel is
     * ASYNCHRONOUS — sd_ant_channel_close() returns immediately and the channel
     * is not actually down until EVENT_CHANNEL_CLOSED arrives, up to a channel
     * period later. Unassigning and re-assigning inside this call therefore
     * fails with NRF_ANT_ERROR_CHANNEL_IN_WRONG_STATE (0x4015) and leaves the
     * sniffer dead, which is exactly what happened the first time this ran: the
     * retune from the wildcard search to the SDM period silently took the
     * channel down and the log went quiet with no error anywhere but the
     * command reply.
     *
     * So: close now, finish in the close event. Deferred, this returns success
     * because the request was accepted — the periodic ANTLOG status is what
     * says whether it is running. */
    if (s_running || s_assigned) {
        s_pending = true;
        if (s_running) {
            (void)sd_ant_channel_close(ANTLOG_CHANNEL);
        } else {
            /* Assigned but never opened: no close event is coming, so the
             * handoff has to happen here. */
            (void)sd_ant_channel_unassign(ANTLOG_CHANNEL);
            s_assigned = false;
            s_pending  = false;
            return apply_config();
        }
        return NRF_SUCCESS;
    }

    return apply_config();
}

uint32_t ant_log_stop(void)
{
    /* A stop cancels a deferred start: otherwise the close this issues would
     * arrive and helpfully re-open the channel the user just asked to close. */
    s_pending = false;

    if (s_running) {
        (void)sd_ant_channel_close(ANTLOG_CHANNEL);
        s_running = false;
        /* The unassign belongs to EVENT_CHANNEL_CLOSED — see ant_log_start().
         * Doing it here races the close and fails, which is harmless in itself
         * but leaves the channel assigned and the next start guessing. */
    } else if (s_assigned) {
        (void)sd_ant_channel_unassign(ANTLOG_CHANNEL);
        s_assigned = false;
    }
    s_locked = false;
    return NRF_SUCCESS;
}

void ant_log_status(char *buf, uint32_t cap)
{
    snprintf(buf, cap,
             "{\"cmd\":\"antlog\",\"running\":%s,\"locked\":%s,\"pending\":%s,"
             "\"cfg\":{\"dev\":%u,\"type\":%u,\"trans\":%u,\"period\":%u,\"freq\":%u},"
             "\"lock\":{\"dev\":%u,\"type\":%u,\"trans\":%u},"
             "\"msgs\":%lu,\"rx_fail\":%lu,\"searches\":%lu}",
             s_running ? "true" : "false", s_locked ? "true" : "false",
             s_pending ? "true" : "false",
             (unsigned)s_cfg_num, (unsigned)s_cfg_type, (unsigned)s_cfg_trans,
             (unsigned)s_cfg_period, (unsigned)s_cfg_freq,
             (unsigned)s_lock_num, (unsigned)s_lock_type, (unsigned)s_lock_trans,
             (unsigned long)s_msgs, (unsigned long)s_rx_fails,
             (unsigned long)s_searches);
}

/* ---- core seam -------------------------------------------------------------
 *
 * Strong overrides of the weak hooks in core/ctrl_dispatch.c, same pattern as
 * the page-70 counters in ant_sdm.c: the command grammar lives in core (which
 * has no radio) and the radio lives here. */
uint32_t ctrl_antlog_start(unsigned dev_num, unsigned dev_type,
                           unsigned trans_type, unsigned period, unsigned freq)
{
    return ant_log_start((uint16_t)dev_num, (uint8_t)dev_type,
                         (uint8_t)trans_type, (uint16_t)period, (uint8_t)freq);
}

uint32_t ctrl_antlog_stop(void)
{
    return ant_log_stop();
}

void ctrl_antlog_status(char *buf, unsigned cap)
{
    ant_log_status(buf, cap);
}
