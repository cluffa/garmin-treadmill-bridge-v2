/*
 * testboard.c — expansion-board test aid implementation.
 *
 * M2 Task 2.4.
 *
 * Compiled only when TESTBOARD=1 (gated by the Makefile SRC_FILES list).
 *
 * Responsibilities:
 *   1. testboard_init() — initialise SSD1306 OLED, status LED, passive buzzer,
 *      and user button; register short- and long-press handlers; start a
 *      ~5 Hz app_timer that drives testboard_render_tick().
 *
 *   2. testboard_render_tick() — read the app_state singleton and compose
 *      8 lines of text on the OLED (21 char/line, 5x7 font), update the RGB
 *      status LED via hw_led_status(), and chirp the buzzer when any tracked
 *      state field changes (link, watch, ANT, fault).
 *
 *   3. Short-press button cycles through four test actions:
 *        SCAN          → weak testboard_action_scan()        (M3 overrides)
 *        CONNECT-NEXT  → weak testboard_action_connect_next() (M3 overrides)
 *        INJECT        → builds a synthetic 15-byte A6ED0004 workout frame
 *                         (8.0 km/h target) and calls workout_ctrl_on_frame()
 *                         so the brain resolves the target immediately.
 *        STOP          → weak testboard_action_stop()         (M3 overrides)
 *
 *   4. Long-press (>= 800 ms) triggers NVIC_SystemReset().
 *
 * Synthetic frame layout (copied from core/workout_ctrl.h):
 *   [ 0] version             = WORKOUT_FRAME_VERSION (1)
 *   [ 1] timerState          = 3  (TIMER_STATE_ON)
 *   [ 2] flags               = 0x01  (FLAG_HAS_STEP)
 *   [ 3] intensity           = 0  (ACTIVE)
 *   [ 4] targetType          = 0  (WORKOUT_STEP_TARGET_SPEED)
 *   [ 5] targetLow  (uint16, LE)  = 2222 mm/s  (  8.0 km/h)
 *   [ 7] targetHigh (uint16, LE)  = 2222 mm/s  (  8.0 km/h)
 *   [ 9] durationType        = 0
 *   [10] durationValue (u32, LE)  = 0
 *   [14] repetitionNumber    = 0
 *
 * Resolved target: midpoint 2222 mm/s →  = 7.999 km/h → ~2.222 m/s.
 */

#include "testboard.h"

#include <stdio.h>
#include <string.h>

#include "app_error.h"
#include "app_state.h"
#include "app_timer.h"
#include "board_pins.h"
#include "hw_buzzer.h"
#include "hw_button.h"
#include "hw_led.h"
#include "nrf_delay.h"
#include "nrf_log.h"
#include "ssd1306.h"
#include "workout_ctrl.h"

/* ---- Render tick interval -------------------------------------------------- */
#define RENDER_INTERVAL_MS  200   /* ~5 Hz */

/* ---- Action cycle ---------------------------------------------------------- */
typedef enum {
    TEST_ACTION_SCAN,
    TEST_ACTION_CONNECT_NEXT,
    TEST_ACTION_INJECT,
    TEST_ACTION_STOP,
    TEST_ACTION_COUNT
} test_action_t;

static test_action_t s_action;

/* ---- State tracking (for buzzer chirps on transitions) --------------------- */
static link_state_t s_prev_link  = LINK_DOWN;
static bool         s_prev_watch = false;
static bool         s_prev_ant   = false;
static uint32_t     s_prev_fault = 0;

/* ---- Render timer ---------------------------------------------------------- */
APP_TIMER_DEF(s_render_timer);

/* ---- Forward declarations -------------------------------------------------- */
static void on_button_short(void);
static void on_button_long(void);
static void render_tick_cb(void *ctx);

/* ---- Weak radio-action stubs (M3 overrides) -------------------------------- */

__attribute__((weak)) void testboard_action_scan(void)
{
    NRF_LOG_INFO("testboard: SCAN (stub — no radio in M2)");
}

__attribute__((weak)) void testboard_action_connect_next(void)
{
    NRF_LOG_INFO("testboard: CONNECT-NEXT (stub — no radio in M2)");
}

__attribute__((weak)) void testboard_action_stop(void)
{
    NRF_LOG_INFO("testboard: STOP (stub — no radio in M2)");
}

/* ---- Synthetic workout-frame injector (8.0 km/h) --------------------------------
 *
 * Frame byte layout (little-endian, 15 bytes) per core/workout_ctrl.h:
 *
 *   Offset  Size  Field              Value   Notes
 *   ------  ----  -----------------  ------  -----------------------------------
 *    0      1     version            1       WORKOUT_FRAME_VERSION
 *    1      1     timerState         3       TIMER_STATE_ON (activity running)
 *    2      1     flags              0x01    FLAG_HAS_STEP (step data present)
 *    3      1     intensity          0       WORKOUT_INTENSITY_ACTIVE
 *    4      1     targetType         0       WORKOUT_STEP_TARGET_SPEED
 *    5-6    2     targetLow (LE)     2222    mm/s (= 8.0 km/h)
 *    7-8    2     targetHigh (LE)    2222    mm/s (= 8.0 km/h)
 *    9      1     durationType       0       (informational only)
 *   10-13   4     durationValue (LE) 0       (informational only)
 *   14      1     repetitionNumber   0       (informational only)
 *
 * workout_ctrl decodes this, resolves to mmps=2222 (= 7.999 km/h, ~2.222 m/s),
 * and calls machine_set_speed() which updates app_state()->resolved_target_mps.
 */

static void inject_synthetic_workout_frame(void)
{
    uint8_t frame[WORKOUT_FRAME_LEN];

    memset(frame, 0, sizeof(frame));

    frame[0] = WORKOUT_FRAME_VERSION;   /* version 1 */
    frame[1] = 3;                       /* timerState = TIMER_STATE_ON */
    frame[2] = 0x01;                    /* flags = FLAG_HAS_STEP */

    /* targetLow = 2222 mm/s (0x08AE LE) */
    frame[5] = 0xAE;
    frame[6] = 0x08;

    /* targetHigh = 2222 mm/s (0x08AE LE) */
    frame[7] = 0xAE;
    frame[8] = 0x08;

    NRF_LOG_INFO("testboard: INJECT synthetic 8.0 km/h workout frame (2222 mm/s)");

    workout_ctrl_on_frame(frame, WORKOUT_FRAME_LEN);
}

/* ---- Button handlers ------------------------------------------------------- */

static void on_button_short(void)
{
    NRF_LOG_INFO("testboard: button short — action %d", (int)s_action);

    switch (s_action) {
    case TEST_ACTION_SCAN:
        hw_buzzer_chirp(800, 60);
        testboard_action_scan();
        break;
    case TEST_ACTION_CONNECT_NEXT:
        hw_buzzer_chirp(1200, 60);
        testboard_action_connect_next();
        break;
    case TEST_ACTION_INJECT:
        hw_buzzer_chirp(2000, 80);
        inject_synthetic_workout_frame();
        break;
    case TEST_ACTION_STOP:
        hw_buzzer_chirp(500, 100);
        testboard_action_stop();
        break;
    case TEST_ACTION_COUNT:
    default:
        /* Sentinel — not a real action; should never be reached because
         * the value is kept within range by the modulo below. */
        break;
    }

    s_action = (s_action + 1) % TEST_ACTION_COUNT;
}

static void on_button_long(void)
{
    NRF_LOG_INFO("testboard: button long — NVIC_SystemReset()");
    hw_buzzer_chirp(400, 200);
    /* Allow the buzzer tone to be audible for a moment before the reset. */
    nrf_delay_ms(250);
    NVIC_SystemReset();
}

/* ---- Initialisation -------------------------------------------------------- */

void testboard_init(void)
{
    /* ---- OLED ----------------------------------------------------------- */
    ssd1306_init();
    ssd1306_clear();
    ssd1306_text(0, 0, "testboard boot...");
    ssd1306_show();

    /* ---- Buzzer --------------------------------------------------------- */
    hw_buzzer_init();

    /* ---- Button --------------------------------------------------------- */
    hw_button_init(on_button_short, on_button_long);

    /* ---- LED: show initial state ---------------------------------------- */
    hw_led_status(app_state()->central_link,
                  app_state()->watch_connected,
                  app_state()->last_fault_code != 0);

    /* ---- Start test action cycle ---------------------------------------- */
    s_action = TEST_ACTION_SCAN;

    /* ---- Render timer (~5 Hz) ------------------------------------------- */
    APP_ERROR_CHECK(app_timer_create(&s_render_timer,
                                     APP_TIMER_MODE_REPEATED,
                                     render_tick_cb));
    APP_ERROR_CHECK(app_timer_start(s_render_timer,
                                    APP_TIMER_TICKS(RENDER_INTERVAL_MS), NULL));

    NRF_LOG_INFO("testboard: init complete (render @ ~5 Hz, button armed)");
}

/* ---- OLED render ----------------------------------------------------------- *
 *
 * 8 rows x 21 columns (6 px per char inc. left padding):
 *
 *   Row 0  link status, watch, ANT
 *   Row 1  belt speed
 *   Row 2  resolved target speed
 *   Row 3  current test action
 *   Row 4  last fault code
 *   Row 5  button hint
 *   Row 6  spare
 *   Row 7  spare
 */

static const char *link_label(link_state_t s)
{
    switch (s) {
    case LINK_DOWN:       return "D";
    case LINK_SCANNING:   return "S";
    case LINK_CONNECTING: return "C";
    case LINK_UP:         return "U";
    default:              return "?";
    }
}

static const char *action_label(test_action_t a)
{
    switch (a) {
    case TEST_ACTION_SCAN:         return "SCAN";
    case TEST_ACTION_CONNECT_NEXT: return "CONN-NEXT";
    case TEST_ACTION_INJECT:       return "INJECT 8.0";
    case TEST_ACTION_STOP:         return "STOP";
    case TEST_ACTION_COUNT:
    default:                       return "?";
    }
}

/* ---- Render tick callback ------------------------------------------------- */

static void render_tick_cb(void *ctx)
{
    (void)ctx;

    app_state_t *st = app_state();
    char line[22];  /* 21 chars + NUL */

    /* ---- Chirp on state transitions ------------------------------------- */
    if (st->central_link != s_prev_link) {
        hw_buzzer_chirp(1500, 40);
        s_prev_link = st->central_link;
    }
    if (st->watch_connected != s_prev_watch) {
        hw_buzzer_chirp(1800, 40);
        s_prev_watch = st->watch_connected;
    }
    if (st->ant_broadcasting != s_prev_ant) {
        hw_buzzer_chirp(2200, 40);
        s_prev_ant = st->ant_broadcasting;
    }
    if (st->last_fault_code != s_prev_fault) {
        /* Fault chirp: lower, longer tone */
        hw_buzzer_chirp(600, 120);
        s_prev_fault = st->last_fault_code;
    }

    /* ---- LED update ----------------------------------------------------- */
    hw_led_status(st->central_link,
                  st->watch_connected,
                  st->last_fault_code != 0);

    /* ---- Compose OLED lines --------------------------------------------- */
    ssd1306_clear();

    /* Row 0: link status + watch + ANT */
    snprintf(line, sizeof(line), "Lnk:%s W:%c A:%c",
             link_label(st->central_link),
             st->watch_connected  ? '+' : '-',
             st->ant_broadcasting ? '+' : '-');
    ssd1306_text(0, 0, line);

    /* Row 1: belt speed */
    snprintf(line, sizeof(line), "Belt:%6.1f km/h",
             (double)st->treadmill.speed_mps * 3.6);
    ssd1306_text(0, 1, line);

    /* Row 2: resolved target speed */
    snprintf(line, sizeof(line), "Targ:%6.1f km/h",
             (double)st->resolved_target_mps * 3.6);
    ssd1306_text(0, 2, line);

    /* Row 3: current test action */
    snprintf(line, sizeof(line), "Act: %-11s", action_label(s_action));
    ssd1306_text(0, 3, line);

    /* Row 4: last fault code */
    snprintf(line, sizeof(line), "Fault: %lu", (unsigned long)st->last_fault_code);
    ssd1306_text(0, 4, line);

    /* Row 5: button hint */
    ssd1306_text(0, 5, "[btn] cycle  long:RST");

    /* Row 6–7: spare */
    ssd1306_text(0, 6, "");

    ssd1306_show();
}
