/*
 * hw_buzzer.c — passive piezo buzzer via nrf_drv_pwm (legacy layer).
 *
 * Uses one PWM instance to drive a square wave on PIN_BUZZER (P0.02).
 * The buzzer is a passive transducer — the PWM frequency sets the tone.
 *
 * Implementation: PWM peripheral, single channel, single-sequence playback.
 * A one-shot app_timer2 stops the PWM after the requested duration.
 *
 * M2 Task 2.2.
 */

#include "hw_buzzer.h"
#include "app_error.h"
#include "app_timer.h"
#include "app_util_platform.h"
#include "board_pins.h"
#include "nrf_drv_pwm.h"
#include "nrf_log.h"
#include "nrf_gpio.h"

/* ---- PWM instance ----------------------------------------------------------- */
static nrf_drv_pwm_t    s_pwm    = NRF_DRV_PWM_INSTANCE(0);
static nrf_pwm_values_individual_t s_duty;
static bool             s_playing;

/* One-shot stop timer */
APP_TIMER_DEF(s_chirp_stop_timer);
static void chirp_stop_cb(void *ctx);

/* ---- Initialise PWM --------------------------------------------------------- */

void hw_buzzer_init(void)
{
    nrf_drv_pwm_config_t cfg = NRF_DRV_PWM_DEFAULT_CONFIG;
    cfg.output_pins[0] = PIN_BUZZER;
    cfg.output_pins[1] = NRF_DRV_PWM_PIN_NOT_USED;
    cfg.output_pins[2] = NRF_DRV_PWM_PIN_NOT_USED;
    cfg.output_pins[3] = NRF_DRV_PWM_PIN_NOT_USED;
    cfg.irq_priority    = APP_IRQ_PRIORITY_LOWEST;
    cfg.base_clock      = NRF_PWM_CLK_1MHz;
    cfg.count_mode      = NRF_PWM_MODE_UP;
    cfg.top_value       = 100;     /* 1% duty resolution */
    cfg.load_mode       = NRF_PWM_LOAD_INDIVIDUAL;
    cfg.step_mode       = NRF_PWM_STEP_AUTO;

    APP_ERROR_CHECK(nrf_drv_pwm_init(&s_pwm, &cfg, NULL));
    s_playing = false;

    /* Create the one-shot stop timer (not started yet). */
    APP_ERROR_CHECK(app_timer_create(&s_chirp_stop_timer,
                                     APP_TIMER_MODE_SINGLE_SHOT,
                                     chirp_stop_cb));
}

/* ---- Chirp ------------------------------------------------------------------ */

static void chirp_stop_cb(void *ctx)
{
    (void)ctx;
    nrf_drv_pwm_stop(&s_pwm, true);  /* wait for idle, force */
    /* Drive output low so the buzzer is silent. */
    nrf_gpio_cfg_output(PIN_BUZZER);
    nrf_gpio_pin_write(PIN_BUZZER, 0);
    s_playing = false;
}

void hw_buzzer_chirp(uint16_t freq_hz, uint16_t ms)
{
    /* Cancel any currently-running chirp. */
    if (s_playing) {
        (void)app_timer_stop(s_chirp_stop_timer);
        nrf_drv_pwm_stop(&s_pwm, true);
        s_playing = false;
    }

    if (freq_hz == 0 || ms == 0) {
        /* Silence. */
        nrf_gpio_cfg_output(PIN_BUZZER);
        nrf_gpio_pin_write(PIN_BUZZER, 0);
        return;
    }

    /*
     * Derive top_value from the desired frequency:
     *   f_pwm = base_clock / top_value
     *   top_value = base_clock / freq_hz
     *
     * We configured base_clock = 1 MHz = 1 000 000 Hz.
     * Duty cycle = 50% → duty = top_value / 2
     */
    uint16_t top = (uint16_t)(1000000UL / freq_hz);
    if (top < 2) top = 2;
    if (top > 10000) top = 10000;

    s_duty.channel_0 = top / 2;
    s_duty.channel_1 = 0;
    s_duty.channel_2 = 0;
    s_duty.channel_3 = 0;

    /*
     * Build a simple sequence: play the configured duty indefinitely.
     * We stop it with the one-shot timer.
     */
    static nrf_pwm_sequence_t seq;
    seq.values.p_individual = &s_duty;
    seq.length              = 1;
    seq.repeats             = 0;
    seq.end_delay           = 0;

    /*
     * Re-init PWM with the frequency-appropriate top.
     * nrf_drv_pwm_init() is re-entrant if the instance is uninitialised
     * first.  Safer: stop, uninit, re-init.
     */
    nrf_drv_pwm_uninit(&s_pwm);

    nrf_drv_pwm_config_t cfg = NRF_DRV_PWM_DEFAULT_CONFIG;
    cfg.output_pins[0] = PIN_BUZZER;
    cfg.output_pins[1] = NRF_DRV_PWM_PIN_NOT_USED;
    cfg.output_pins[2] = NRF_DRV_PWM_PIN_NOT_USED;
    cfg.output_pins[3] = NRF_DRV_PWM_PIN_NOT_USED;
    cfg.irq_priority    = APP_IRQ_PRIORITY_LOWEST;
    cfg.base_clock      = NRF_PWM_CLK_1MHz;
    cfg.count_mode      = NRF_PWM_MODE_UP;
    cfg.top_value       = top;
    cfg.load_mode       = NRF_PWM_LOAD_INDIVIDUAL;
    cfg.step_mode       = NRF_PWM_STEP_AUTO;

    (void)nrf_drv_pwm_init(&s_pwm, &cfg, NULL);

    APP_ERROR_CHECK(nrf_drv_pwm_simple_playback(&s_pwm, &seq, 1,
                    NRF_DRV_PWM_FLAG_LOOP));
    s_playing = true;

    /* Schedule stop after `ms` milliseconds. */
    APP_ERROR_CHECK(app_timer_start(s_chirp_stop_timer,
                                    APP_TIMER_TICKS(ms), NULL));
}
