/*
 * main.c — XIAO nRF52840 treadmill bridge, firmware bring-up.
 *
 * SoftDevice up (S340), app_timer, NRF_LOG over RTT, USB-CDC ACM console
 * + interactive ctrl dispatch, nrf_pwr_mgmt, idle heartbeat.
 *
 * When TESTBOARD=1 the expansion-board test aid (OLED, LED, buzzer, button)
 * is initialised and a ~5 Hz render tick drives the display. M3 Task 3.1 adds
 * the BLE peripheral ctrl-svc (watch facing); central + ANT come later.
 */

#include <stdint.h>
#include <stdio.h>

#include "app_error.h"
#include "app_timer.h"
#include "app_usbd.h"
#include "ble_ctrl_svc.h"
#include "boards.h"
#include "nrf_drv_clock.h"
#include "nrf_log.h"
#include "nrf_log_ctrl.h"
#include "nrf_log_default_backends.h"
#include "nrf_pwr_mgmt.h"
#include "nrf_sdh.h"
#include "nrf_sdh_ant.h"
#include "nrf_sdh_ble.h"

#include "ant_sdm.h"
#include "app_state.h"
#include "ble_central.h"
#include "ftms_devlist.h"
#include "machine.h"
#include "usb_cdc_log.h"
#include "workout_ctrl.h"

#if TESTBOARD
#include "testboard/testboard.h"
#endif

#define HEARTBEAT_MS 1000

APP_TIMER_DEF(m_heartbeat_timer);

static uint32_t s_heartbeat_cnt;

static void heartbeat_cb(void *ctx)
{
    (void)ctx;
    unsigned int n = (unsigned int)s_heartbeat_cnt++;

    NRF_LOG_INFO("alive %u", n);

    /* ~1 Hz control keepalive: re-assert the resolved target speed if a write
     * to the treadmill was lost. The watch only sends the workout frame on
     * change, so without this a dropped command (BLE noise, S340 timeslot
     * jitter, an iFit write outside its poll slot) would never self-heal —
     * the belt would sit at the wrong speed. This is the core of the
     * device-is-the-brain policy; workout_ctrl_tick() is a no-op until a
     * frame has latched a target. */
    workout_ctrl_tick();

    /* Push the treadmill link state to a subscribed watch on change so the
     * picker updates without polling for STATUS. */
    static bool s_last_link;
    bool link = machine_connected();
    if (link != s_last_link) {
        s_last_link = link;
        ble_ctrl_svc_notify_status();
    }

    /* Also route the heartbeat to USB-CDC so the console shows signs of
     * life with no J-Link / RTT viewer attached. */
    char buf[32];
    snprintf(buf, sizeof(buf), "alive %u", n);
    usb_cdc_log_write(buf);
}

static void log_init(void)
{
    APP_ERROR_CHECK(NRF_LOG_INIT(NULL));
    NRF_LOG_DEFAULT_BACKENDS_INIT();
}

static void softdevice_init(void)
{
    APP_ERROR_CHECK(nrf_sdh_enable_request());
    ASSERT(nrf_sdh_is_enabled());
}

static void ble_stack_init(void)
{
    uint32_t ram_start = 0;
    APP_ERROR_CHECK(nrf_sdh_ble_default_cfg_set(1 /* conn_cfg_tag */,
                                                &ram_start));
    APP_ERROR_CHECK(nrf_sdh_ble_enable(&ram_start));
    NRF_LOG_INFO("BLE stack enabled");
}

static void ant_stack_init(void)
{
    APP_ERROR_CHECK(nrf_sdh_ant_enable());
    NRF_LOG_INFO("ANT stack enabled");
}

static void timers_init(void)
{
    APP_ERROR_CHECK(nrf_drv_clock_init());
    nrf_drv_clock_lfclk_request(NULL);
    APP_ERROR_CHECK(app_timer_init());
    APP_ERROR_CHECK(app_timer_create(&m_heartbeat_timer, APP_TIMER_MODE_REPEATED,
                                     heartbeat_cb));
    APP_ERROR_CHECK(app_timer_start(m_heartbeat_timer,
                                    APP_TIMER_TICKS(HEARTBEAT_MS), NULL));
}

int main(void)
{
    log_init();
    timers_init();
    softdevice_init();
    ble_stack_init();
    ant_stack_init();
    APP_ERROR_CHECK(nrf_pwr_mgmt_init());

    NRF_LOG_INFO("xiao-nrf52840 up, S340 present");

    /* Shared device state (used by radios + testboard) */
    app_state_init();

    /* Watch-facing BLE peripheral: control service + advertising */
    ble_ctrl_svc_init();
    ble_ctrl_svc_advertise_start();
    NRF_LOG_INFO("ctrl_svc: initialized and advertising");

    /* Treadmill-facing BLE central: scanning, connect, data, control writes */
    ble_central_init();
    NRF_LOG_INFO("ble_central: initialized");

    /* ANT+ SDM master: footpod broadcast (watch reads speed natively) */
    ant_sdm_init();
    ant_sdm_start();
    NRF_LOG_INFO("ant_sdm: initialized and broadcasting");

    /* USB-CDC ACM: log console + interactive ctrl command dispatch */
    usb_cdc_log_init();
    NRF_LOG_INFO("USB-CDC initialized");

#if TESTBOARD
    /* Expansion-board test aid: OLED, LED, buzzer, button — optional (M2). */
    testboard_init();
    NRF_LOG_INFO("testboard initialized (OLED + LED + buzzer + button)");
#endif

    for (;;) {
        /* Pump USBD events (CDC ACM RX/TX callbacks fire here) */
        (void)app_usbd_event_queue_process();

        if (!NRF_LOG_PROCESS()) {
            nrf_pwr_mgmt_run();
        }
    }
}

/*
 * Override the TESTBOARD weak stubs with real radio actions (M3).
 * When TESTBOARD=1, the button short-press action cycler calls these.
 */

void testboard_action_scan(void)
{
    NRF_LOG_INFO("testboard: SCAN (live)");
    ble_central_scan_start();
}

void testboard_action_connect_next(void)
{
    static int s_next_idx;
    ftms_device_t devs[FTMS_MAX_DEVICES];
    int n = machine_get_devices(devs, FTMS_MAX_DEVICES);
    if (n == 0) {
        NRF_LOG_INFO("testboard: CONNECT-NEXT — no devices in list");
        return;
    }
    int idx = s_next_idx % n;
    s_next_idx++;
    NRF_LOG_INFO("testboard: CONNECT-NEXT idx %d", idx);
    ble_central_connect(idx);
}

void testboard_action_stop(void)
{
    NRF_LOG_INFO("testboard: STOP (live)");
    machine_stop();
}
