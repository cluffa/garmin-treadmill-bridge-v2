/*
 * hw_led.c — onboard RGB LED (active-low GPIO).
 *
 * Pin assignments from board_pins.h:
 *   Red   PIN_LED_RED    P0.26
 *   Green PIN_LED_GREEN  P0.30
 *   Blue  PIN_LED_BLUE   P0.06
 *
 * M2 Task 2.2.
 */

#include "hw_led.h"
#include "board_pins.h"
#include "nrf_gpio.h"

/* ---- Raw control ------------------------------------------------------------ */

void hw_led_set(bool r, bool g, bool b)
{
    /* Active-low: write 0 to turn ON, 1 to turn OFF. */
    nrf_gpio_pin_write(PIN_LED_RED,   r ? 0 : 1);
    nrf_gpio_pin_write(PIN_LED_GREEN, g ? 0 : 1);
    nrf_gpio_pin_write(PIN_LED_BLUE,  b ? 0 : 1);
}

/* ---- Initialisation --------------------------------------------------------- *
 *
 * Called once (automatically by hw_led_status on first use) to configure the
 * three LED pins as outputs with LEDs off.
 */
static bool s_led_inited;

static void ensure_inited(void)
{
    if (s_led_inited) return;
    s_led_inited = true;

    nrf_gpio_cfg_output(PIN_LED_RED);
    nrf_gpio_cfg_output(PIN_LED_GREEN);
    nrf_gpio_cfg_output(PIN_LED_BLUE);

    /* Start off (logic 1 = off for active-low). */
    nrf_gpio_pin_write(PIN_LED_RED,   1);
    nrf_gpio_pin_write(PIN_LED_GREEN, 1);
    nrf_gpio_pin_write(PIN_LED_BLUE,  1);
}

/* ---- Status mapping --------------------------------------------------------- */

void hw_led_status(link_state_t central, bool watch, bool fault)
{
    ensure_inited();

    /* Fault overrides everything. */
    if (fault) {
        hw_led_set(true, false, false); /* red */
        return;
    }

    switch (central) {
    case LINK_DOWN:
        hw_led_set(false, false, false); /* off */
        break;
    case LINK_SCANNING:
        /* Fast blue blink — for now just solid blue; testboard render tick
         * (Task 2.4) can drive actual blinking via a periodic timer. */
        hw_led_set(false, false, true);  /* blue */
        break;
    case LINK_CONNECTING:
        /* Yellow = red + green */
        hw_led_set(true, true, false);   /* yellow */
        break;
    case LINK_UP:
        if (watch) {
            hw_led_set(false, true, true); /* cyan = green + blue */
        } else {
            hw_led_set(false, true, false); /* green */
        }
        break;
    }
}
