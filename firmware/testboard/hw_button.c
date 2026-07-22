/*
 * hw_button.c — debounced button driver using SDK app_button + app_timer.
 *
 * One button on PIN_BUTTON (P0.03):
 *   - app_button debounce period: 50 ms
 *   - Long-press threshold: 800 ms, detected via app_timer
 *
 * The app_button module fires on every RELEASE (short press).
 * We also start a one-shot timer on each PRESS; if it expires before
 * release, we fire the long-press callback and suppress the short-press
 * callback for that press.
 *
 * M2 Task 2.2.
 */

#include "hw_button.h"
#include "app_button.h"
#include "app_error.h"
#include "app_timer.h"
#include "board_pins.h"
#include "nrf_log.h"

#define DEBOUNCE_MS      50
#define LONG_PRESS_MS    800

static void (*s_on_short)(void);
static void (*s_on_long)(void);

static bool     s_long_fired;         /* true once long cb fired this press */

APP_TIMER_DEF(s_long_timer);
static void long_timer_cb(void *ctx);

/* ---- app_button event handler ----------------------------------------------- */

static void button_handler(uint8_t pin_no, uint8_t button_action)
{
    (void)pin_no;

    if (button_action == APP_BUTTON_PUSH) {
        /* Press: start the long-press timer. */
        s_long_fired = false;
        (void)app_timer_stop(s_long_timer);
        APP_ERROR_CHECK(app_timer_start(s_long_timer,
                                        APP_TIMER_TICKS(LONG_PRESS_MS), NULL));
    } else if (button_action == APP_BUTTON_RELEASE) {
        /* Release: stop the long-press timer. */
        (void)app_timer_stop(s_long_timer);

        if (!s_long_fired && s_on_short) {
            s_on_short();
        }
    }
}

static void long_timer_cb(void *ctx)
{
    (void)ctx;
    s_long_fired = true;
    if (s_on_long) {
        s_on_long();
    }
}

/* ---- Public init ------------------------------------------------------------ */

void hw_button_init(void (*on_short)(void), void (*on_long)(void))
{
    s_on_short = on_short;
    s_on_long  = on_long;
    s_long_fired = false;

    /* Create the long-press detection timer. */
    APP_ERROR_CHECK(app_timer_create(&s_long_timer,
                                     APP_TIMER_MODE_SINGLE_SHOT,
                                     long_timer_cb));

    /* Configure the single button. */
    static app_button_cfg_t buttons[] = {
        {
            .pin_no            = PIN_BUTTON,
            .active_state      = 0,           /* active low */
            .pull_cfg          = NRF_GPIO_PIN_PULLUP,
            .button_handler    = button_handler,
        },
    };

    APP_ERROR_CHECK(app_button_init(buttons, ARRAY_SIZE(buttons),
                                    DEBOUNCE_MS));
    APP_ERROR_CHECK(app_button_enable());
}
