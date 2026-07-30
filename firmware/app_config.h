#pragma once
/*
 * app_config.h — project overrides applied on top of the nRF5 SDK template
 * sdk_config.h (the Makefile defines USE_APP_CONFIG, which every SDK
 * sdk_config.h template honors by including this file first).
 *
 * THIS FILE ALWAYS WINS. Every value in sdk_config.h is #ifndef-guarded, so a
 * setting defined here silently overrides whatever sdk_config.h says. Editing
 * sdk_config.h to change something defined below has NO effect — change it
 * here. (f3e201f learned this the hard way with the LF clock.)
 *
 * Minimal boot skeleton (Task 1.1): SoftDevice enable, app_timer, NRF_LOG,
 * nrf_pwr_mgmt, idle heartbeat. No BLE/ANT/testboard yet.
 */

/* ---- SoftDevice handler: BLE + ANT both enabled under S340 ----------------- */
#define NRF_SDH_ENABLED 1
#define NRF_SDH_BLE_ENABLED 1
#define NRF_SDH_ANT_ENABLED 1
#define NRF_SDH_SOC_ENABLED 1

/* BLE peripheral + central: watch ctrl service (1 link) + treadmill (1 link). */
#define NRF_SDH_BLE_PERIPHERAL_LINK_COUNT 1
#define NRF_SDH_BLE_CENTRAL_LINK_COUNT 1
#define NRF_SDH_BLE_TOTAL_LINK_COUNT 2
#define NRF_SDH_BLE_VS_UUID_COUNT 2        /* ctrl-svc base + iFit vendor base */
#define NRF_SDH_BLE_GATT_MAX_MTU_SIZE 23    /* CIQ default MTU */

/* BLE central: scanning module */
#define NRF_BLE_SCAN_ENABLED 1
#define NRF_BLE_SCAN_BUFFER 255

/* Scan duty cycle. Units are 0.625 ms, so 160/32 is a 20 ms window every 100 ms
 * = 20%. The SDK default is 160/80 — a 50% duty cycle, which assumes scanning is
 * the only thing the radio is doing. Here it is one of three users: a BLE
 * peripheral link to the watch, a BLE central link to the treadmill, and an ANT
 * master broadcasting at 8 Hz. At 50% the peripheral link starved and dropped
 * with BLE_HCI_CONNECTION_TIMEOUT (0x08) while the central was mid-connect.
 * Discovery gets slower, which costs a second or two on SCAN and is worth it to
 * keep an established link alive. */
#define NRF_BLE_SCAN_SCAN_INTERVAL 160   /* 100 ms */
#define NRF_BLE_SCAN_SCAN_WINDOW    32   /*  20 ms */

/* Treadmill link connection interval, in ms. The SDK default minimum is 7.5 ms
 * — the fastest interval BLE permits — which lets a central link claim a radio
 * event every 7.5 ms. With a 30 ms peripheral link to the watch and an ANT
 * master broadcasting at 8 Hz also competing, the peripheral link starved and
 * dropped with BLE_HCI_CONNECTION_TIMEOUT (0x08) whenever the treadmill link
 * was active. The peripheral is the fragile one: the central negotiates a 4 s
 * supervision timeout while the watch's central picks its own, and macOS picks
 * 720 ms and ignores our PPCP.
 *
 * A treadmill streams telemetry at 1-4 Hz, so 7.5 ms buys nothing and costs the
 * link we cannot afford to lose. */
#define NRF_BLE_SCAN_MIN_CONNECTION_INTERVAL 30
#define NRF_BLE_SCAN_MAX_CONNECTION_INTERVAL 60
#define NRF_BLE_SCAN_FILTER_ENABLE 0         /* we classify in SW */
#define NRF_BLE_SCAN_CONNECTION_ENABLE 0     /* connect_policy owns connects */
#define NRF_BLE_SCAN_NAME_CNT 0
#define NRF_BLE_SCAN_SHORT_NAME_CNT 0
#define NRF_BLE_SCAN_ADDRESS_CNT 0
#define NRF_BLE_SCAN_UUID_CNT 0
#define NRF_BLE_SCAN_APPEARANCE_CNT 0

/* Peer Manager: enable central features for bond support */
#define PM_CENTRAL_ENABLED 1

/* ---- ANT stack configuration (S340 SoftDevice) ------------------------------- */
#define NRF_SDH_ANT_TOTAL_CHANNELS_ALLOCATED 1   /* SDM master channel */
#define NRF_SDH_ANT_ENCRYPTED_CHANNELS 0          /* no encrypted channels */
#define NRF_SDH_ANT_BURST_QUEUE_SIZE 128
#define NRF_SDH_ANT_EVENT_QUEUE_SIZE 32
#define NRF_SDH_ANT_OBSERVER_PRIO_LEVELS 2
#define NRF_SDH_ANT_STACK_OBSERVER_PRIO 0

/* ---- clock: 32.768 kHz LFXO ------------------------------------------------
 *
 * The XIAO's LFXO works. The earlier "crystal does not oscillate" finding was a
 * measurement error: it polled EVENTS_LFCLKSTARTED (0x40000104), a self-clearing
 * one-shot latch that the SDH clock handler clears once serviced, rather than
 * LFCLKSTAT.STATE (0x40000418 bit 16). Verify with LFCLKSTAT, which must read
 * SRC=1 (Xtal), STATE=1 (Running) -> 0x00010001.
 *
 * XTAL matters here beyond tidiness: LF accuracy sets BLE connection-event and
 * ANT channel timing margin, and this device runs BLE peripheral + BLE central
 * + ANT master concurrently. 500 ppm (RC) vs 20 ppm (XTAL) is the difference
 * between comfortable and marginal in that three-radio window.
 *
 * The RC_CTIV values below are retained but inert (the SoftDevice only uses
 * them when LF_SRC is RC), so falling back is a one-line change. */
#define NRF_SDH_CLOCK_LF_SRC 1        /* XTAL */
#define NRF_SDH_CLOCK_LF_RC_CTIV 0    /* must be 0 for XTAL */
#define NRF_SDH_CLOCK_LF_RC_TEMP_CTIV 0
#define NRF_SDH_CLOCK_LF_ACCURACY 7   /* NRF_CLOCK_LF_ACCURACY_20_PPM */

/* ---- clock driver (app_timer LFCLK request; SDH takes over once SD is up) - */
#define NRFX_CLOCK_ENABLED 1
#define NRF_CLOCK_ENABLED 1                  /* legacy nrf_drv_clock alias */
#define NRFX_CLOCK_CONFIG_LF_SRC 1           /* XTAL, matches NRF_SDH_CLOCK_LF_SRC */
#define CLOCK_CONFIG_LF_SRC 1
#define NRFX_CLOCK_CONFIG_IRQ_PRIORITY 6
#define CLOCK_CONFIG_IRQ_PRIORITY 6
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
/* Deferred logging is REQUIRED, not a preference. With NRF_LOG_DEFERRED 0 every
 * NRF_LOG_* call dequeues synchronously into the backends at the call site —
 * and the CDC backend's put() calls app_usbd_cdc_acm_write(). usbd_user_ev_handler()
 * logs from inside app_usbd's own event callbacks (APP_USBD_EVT_POWER_DETECTED,
 * POWER_READY, STARTED), so the backend re-entered app_usbd mid-dispatch —
 * before app_usbd_enable()/app_usbd_start() had even run, and with interrupts
 * masked by cdc_tx_raw()'s critical region. Enumeration never got past the
 * device/string descriptors. Deferring moves the backend write out to
 * NRF_LOG_PROCESS() in the main loop, which breaks the re-entrancy. */
#define NRF_LOG_DEFERRED 1
#define NRF_LOG_BUFSIZE 2048   /* boot logs a lot before the main loop drains it */

/* The RTT up-buffer is the only console on this board (USB CDC is broken), and
 * the SDK backend runs it in non-blocking SKIP mode: once full with nothing
 * draining it, every later message is silently discarded. At the 512-byte
 * default the boot log filled at "ant_sdm init" and everything after it — the
 * USB state-machine events, and any fatal error — was invisible. */
#define SEGGER_RTT_CONFIG_BUFFER_SIZE_UP 8192
#define NRF_LOG_BACKEND_RTT_ENABLED 1
#define NRF_LOG_BACKEND_UART_ENABLED 0
#define NRF_LOG_STR_PUSH_BUFFER_SIZE 128
#define NRF_FPRINTF_ENABLED 1
#define NRF_FPRINTF_FLAG_AUTOMATIC_CR_ON_LF_ENABLED 1

/* ---- USB-CDC ACM console (probe-free debug) ------------------------------- */
#define APP_USBD_ENABLED 1
#define APP_USBD_CDC_ACM_ENABLED 1
#define NRFX_USBD_ENABLED 1
#define USBD_ENABLED 1
#define NRFX_POWER_ENABLED 1
#define POWER_ENABLED 1

#define APP_USBD_VID  0x1915               /* Nordic Semiconductor vendor ID */
#define APP_USBD_PID  0x521F               /* arbitrary, non-conflicting */
#define APP_USBD_DEVICE_VER_MAJOR 1
#define APP_USBD_DEVICE_VER_MINOR 0
#define APP_USBD_DEVICE_VER_SUB   0
#define APP_USBD_CONFIG_SELF_POWERED 0     /* bus-powered */
#define APP_USBD_CONFIG_MAX_POWER 100
#define APP_USBD_CONFIG_EVENT_QUEUE_ENABLE 1
#define APP_USBD_CONFIG_EVENT_QUEUE_SIZE 32
#define APP_USBD_CONFIG_POWER_EVENTS_PROCESS 1

#define APP_USBD_STRING_ID_MANUFACTURER  1
#define APP_USBD_STRING_ID_PRODUCT       2
#define APP_USBD_STRING_ID_SERIAL        3
#define APP_USBD_STRING_ID_CONFIGURATION 4

#define APP_USBD_CONFIG_DESC_STRING_SIZE 31
#define APP_USBD_STRINGS_LANGIDS APP_USBD_LANG_AND_SUBLANG(APP_USBD_LANG_ENGLISH, APP_USBD_SUBLANG_ENGLISH_US)

#define APP_USBD_STRINGS_MANUFACTURER_EXTERN 0
#define APP_USBD_STRINGS_MANUFACTURER  APP_USBD_STRING_DESC("Nordic Semiconductor")
#define APP_USBD_STRINGS_PRODUCT_EXTERN 0
#define APP_USBD_STRINGS_PRODUCT       APP_USBD_STRING_DESC("Garmin Treadmill Bridge")
#define APP_USBD_STRING_SERIAL_EXTERN 1
#define APP_USBD_STRING_SERIAL         g_extern_serial_number
#define APP_USBD_STRING_CONFIGURATION_EXTERN 0
#define APP_USBD_STRINGS_CONFIGURATION APP_USBD_STRING_DESC("CDC ACM UART")
#define APP_USBD_STRINGS_USER X(APP_USER_1, , APP_USBD_STRING_DESC(""))

#define APP_USBD_CDC_ACM_ZLP_ON_EPSIZE_WRITE 0

#define NRFX_USBD_CONFIG_IRQ_PRIORITY 6
#define NRFX_USBD_CONFIG_DMASCHEDULER_MODE 0
#define NRFX_USBD_CONFIG_DMASCHEDULER_ISO_BOOST 0
#define NRFX_USBD_CONFIG_ISO_IN_ZLP 0
#define NRFX_USBD_USE_WORKAROUND_FOR_ANOMALY_211 0

#define USBD_CONFIG_IRQ_PRIORITY 6
#define USBD_CONFIG_DMASCHEDULER_MODE 0
#define USBD_CONFIG_DMASCHEDULER_ISO_BOOST 0
#define USBD_CONFIG_ISO_IN_ZLP 0

#define POWER_CONFIG_IRQ_PRIORITY 6
#define NRFX_POWER_CONFIG_IRQ_PRIORITY 6
#define NRFX_POWER_CONFIG_DEFAULT_DCDCEN 0
#define NRFX_POWER_DEFAULT_DCDCEN 0
#define NRFX_POWER_CONFIG_DEFAULT_DCDCENHV 0

/* ---- TWI (I2C) for SSD1306 OLED ------------------------------------------- */
#define NRFX_TWI_ENABLED 1
#define NRFX_TWIM_ENABLED 1
#define TWI_ENABLED 1
#define TWI0_ENABLED 1
#define TWI0_USE_EASY_DMA 0
#define TWI0_CONFIG_FREQUENCY    NRF_TWI_FREQ_400K
#define TWI0_CONFIG_IRQ_PRIORITY APP_IRQ_PRIORITY_LOWEST
#define NRFX_TWI_DEFAULT_CONFIG_FREQUENCY     NRF_TWI_FREQ_400K
#define NRFX_TWI_DEFAULT_CONFIG_IRQ_PRIORITY  APP_IRQ_PRIORITY_LOWEST
#define NRFX_TWI_DEFAULT_CONFIG_HOLD_BUS_UNINIT 0
#define NRFX_TWIM_DEFAULT_CONFIG_FREQUENCY     NRF_TWI_FREQ_400K
#define NRFX_TWIM_DEFAULT_CONFIG_IRQ_PRIORITY  6
#define NRFX_TWIM_DEFAULT_CONFIG_HOLD_BUS_UNINIT 0
#define TWI_DEFAULT_CONFIG_FREQUENCY    NRF_TWI_FREQ_400K
#define TWI_DEFAULT_CONFIG_IRQ_PRIORITY APP_IRQ_PRIORITY_LOWEST
#define TWI_DEFAULT_CONFIG_CLR_BUS_INIT 0
#define TWI_DEFAULT_CONFIG_HOLD_BUS_UNINIT 0

/* ---- GPIOTE (already in sdk_config.h; app_button pulls it) ----------------- */
#define GPIOTE_ENABLED 1
#define NRFX_GPIOTE_ENABLED 1

/* ---- PWM for passive buzzer ------------------------------------------------ */
#define NRFX_PWM_ENABLED 1
#define NRFX_PWM0_ENABLED 1
#define PWM_ENABLED 1
#define PWM0_ENABLED 1
#define APP_PWM_ENABLED 1

/* nrfx_pwm default config — required by NRFX_PWM_DEFAULT_CONFIG macro */
#define NRFX_PWM_DEFAULT_CONFIG_OUT0_PIN       NRFX_PWM_PIN_NOT_USED
#define NRFX_PWM_DEFAULT_CONFIG_OUT1_PIN       NRFX_PWM_PIN_NOT_USED
#define NRFX_PWM_DEFAULT_CONFIG_OUT2_PIN       NRFX_PWM_PIN_NOT_USED
#define NRFX_PWM_DEFAULT_CONFIG_OUT3_PIN       NRFX_PWM_PIN_NOT_USED
#define NRFX_PWM_DEFAULT_CONFIG_IRQ_PRIORITY   APP_IRQ_PRIORITY_LOWEST
#define NRFX_PWM_DEFAULT_CONFIG_BASE_CLOCK     NRF_PWM_CLK_1MHz
#define NRFX_PWM_DEFAULT_CONFIG_COUNT_MODE     NRF_PWM_MODE_UP
#define NRFX_PWM_DEFAULT_CONFIG_TOP_VALUE      100
#define NRFX_PWM_DEFAULT_CONFIG_LOAD_MODE      NRF_PWM_LOAD_COMMON
#define NRFX_PWM_DEFAULT_CONFIG_STEP_MODE      NRF_PWM_STEP_AUTO
#define NRFX_PWM0_CONFIG_IRQ_PRIORITY          APP_IRQ_PRIORITY_LOWEST

/* ---- app_button (debounced expansion board button) ------------------------- */
#define BUTTON_ENABLED 1

/* ---- app_timer (already enabled; app_pwm needs it) ------------------------- */
#define APP_TIMER_ENABLED 1

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
