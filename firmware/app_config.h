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
#define NRF_LOG_DEFERRED 0
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
