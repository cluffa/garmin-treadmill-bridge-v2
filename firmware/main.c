/*
 * main.c — XIAO nRF52840 treadmill bridge, firmware bring-up.
 *
 * SoftDevice up (S340), app_timer, NRF_LOG over RTT, USB-CDC ACM console
 * + interactive ctrl dispatch, nrf_pwr_mgmt, idle heartbeat.
 *
 * When TESTBOARD=1 the expansion-board test aid (OLED, LED, buzzer, button)
 * is initialised and a ~5 Hz render tick drives the display. No radios yet
 * (M3 adds BLE central / ctrl svc / ANT).
 */

#include <stdint.h>
#include <stdio.h>

#include "app_error.h"
#include "app_timer.h"
#include "app_usbd.h"
#include "boards.h"
#include "nrf_drv_clock.h"
#include "nrf_log.h"
#include "nrf_log_ctrl.h"
#include "nrf_log_default_backends.h"
#include "nrf_pwr_mgmt.h"
#include "nrf_sdh.h"

#include "usb_cdc_log.h"

#if TESTBOARD
#include "app_state.h"
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
    APP_ERROR_CHECK(nrf_pwr_mgmt_init());

    NRF_LOG_INFO("xiao-nrf52840 up, S340 present");

    /* USB-CDC ACM: log console + interactive ctrl command dispatch */
    usb_cdc_log_init();
    NRF_LOG_INFO("USB-CDC initialized");

#if TESTBOARD
    /* Expansion-board test aid: OLED, LED, buzzer, button — optional (M2). */
    app_state_init();
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
