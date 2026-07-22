#pragma once
/*
 * app_config.h — project overrides applied on top of the nRF5 SDK template
 * sdk_config.h (the Makefile defines USE_APP_CONFIG, which every SDK
 * sdk_config.h template honors by including this file first).
 *
 * Minimal boot skeleton (Task 1.1): SoftDevice enable, app_timer, NRF_LOG,
 * nrf_pwr_mgmt, idle heartbeat. No BLE/ANT/testboard yet.
 */

/* ---- SoftDevice handler: enabled (BLE + ANT disabled for now) --------------- */
#define NRF_SDH_ENABLED 1
#define NRF_SDH_BLE_ENABLED 0
#define NRF_SDH_ANT_ENABLED 0
#define NRF_SDH_SOC_ENABLED 1

/* ---- clock: XIAO has a 32.768 kHz crystal -------------------------------- */
#define NRF_SDH_CLOCK_LF_SRC 1        /* XTAL */
#define NRF_SDH_CLOCK_LF_RC_CTIV 0
#define NRF_SDH_CLOCK_LF_RC_TEMP_CTIV 0
#define NRF_SDH_CLOCK_LF_ACCURACY 7   /* 20 ppm */

/* ---- clock driver (app_timer LFCLK request; SDH takes over once SD is up) - */
#define NRFX_CLOCK_ENABLED 1
#define NRF_CLOCK_ENABLED 1                  /* legacy nrf_drv_clock alias */
#define NRFX_CLOCK_CONFIG_LF_SRC 1           /* XTAL, matches NRF_SDH_CLOCK_LF_SRC */
#define CLOCK_CONFIG_LF_SRC 1
#define NRFX_CLOCK_CONFIG_IRQ_PRIORITY 6
#define NRFX_CLOCK_CONFIG_LF_CAL_ENABLED 0
#define CLOCK_CONFIG_SOC_OBSERVER_PRIO 0
#define CLOCK_CONFIG_STATE_OBSERVER_PRIO 0
#define CLOCK_CONFIG_LOG_ENABLED 0

/* ---- libraries ------------------------------------------------------------ */
#define APP_TIMER_ENABLED 1
#define APP_TIMER_CONFIG_RTC_FREQUENCY 0     /* 32768 Hz */
#define APP_TIMER_CONFIG_OP_QUEUE_SIZE 10
#define NRF_PWR_MGMT_ENABLED 1
#define NRF_QUEUE_ENABLED 1
#define NRF_SECTION_ITER_ENABLED 1
#define NRF_SORTLIST_ENABLED 1               /* app_timer2 */

/* ---- logging over SEGGER RTT ---------------------------------------------- */
#define NRF_LOG_ENABLED 1
#define NRF_LOG_DEFAULT_LEVEL 3              /* info */
#define NRF_LOG_DEFERRED 0
#define NRF_LOG_BACKEND_RTT_ENABLED 1
#define NRF_LOG_BACKEND_UART_ENABLED 0
#define NRF_LOG_STR_PUSH_BUFFER_SIZE 128
#define NRF_FPRINTF_ENABLED 1
#define NRF_FPRINTF_FLAG_AUTOMATIC_CR_ON_LF_ENABLED 1

/* ---- FDS (needed by nrf_fstorage which is pulled by other modules) -------- */
#define FDS_ENABLED 1
#define FDS_VIRTUAL_PAGES 2
#define FDS_VIRTUAL_PAGE_SIZE 1024
#define FDS_OP_QUEUE_SIZE 4
#define FDS_MAX_USERS 2
#define NRF_FSTORAGE_ENABLED 1
#define NRF_FSTORAGE_SD_QUEUE_SIZE 4
#define NRF_FSTORAGE_SD_MAX_RETRIES 8
#define NRF_FSTORAGE_SD_MAX_WRITE_SIZE 4096
