#pragma once
/*
 * board_pins.h — complete XIAO nRF52840 + expansion board pin map.
 *
 * Sources:
 *   - Seeed XIAO nRF52840 schematic / wiki: https://wiki.seeedstudio.com/XIAO_BLE/
 *   - Seeed XIAO expansion board silkscreen
 *
 * This file includes the SDK-required custom_board.h and adds project-specific
 * pin definitions for peripherals on the expansion board.
 */

#include "custom_board.h"

/* ---- Onboard RGB LED (active LOW) ------------------------------------------- */
#define PIN_LED_RED    NRF_GPIO_PIN_MAP(0, 26)
#define PIN_LED_GREEN  NRF_GPIO_PIN_MAP(0, 30)
#define PIN_LED_BLUE   NRF_GPIO_PIN_MAP(0, 6)

/* ---- Expansion board: I2C (OLED SSD1306 @ 0x3C) ----------------------------- */
#define PIN_I2C_SDA    NRF_GPIO_PIN_MAP(0, 4)   /* D4 */
#define PIN_I2C_SCL    NRF_GPIO_PIN_MAP(0, 5)   /* D5 */

/* ---- Expansion board: button ------------------------------------------------ */
#define PIN_BUTTON     NRF_GPIO_PIN_MAP(0, 3)   /* D1, pulled high, active low */

/* ---- Expansion board: passive buzzer ---------------------------------------- */
#define PIN_BUZZER     NRF_GPIO_PIN_MAP(0, 2)   /* D3 / A3, PWM */
