#pragma once
/*
 * Board definition for the Seeed XIAO nRF52840 (nRF5 SDK BOARD_CUSTOM).
 *
 * Pin map source: Seeed XIAO nRF52840 schematic / wiki page
 *   https://wiki.seeedstudio.com/XIAO_BLE/
 *
 * Onboard RGB user LED (active LOW):
 *   red   P0.26
 *   green P0.30
 *   blue  P0.06
 *
 * XIAO expansion board:
 *   OLED I2C:  SDA = D4 = P0.04, SCL = D5 = P0.05
 *   Button:    D1 = P0.03
 *   Buzzer:    D3 = P0.02 (passive buzzer, PWM)
 *
 * UART on castellated pins: TX = P1.11 (D6), RX = P1.12 (D7)
 * SWD is on the back test pads (SWDIO/SWCLK/RESET/GND/3V3).
 */

#include "nrf_gpio.h"

#define LEDS_NUMBER    3

#define LED_1          NRF_GPIO_PIN_MAP(0, 26)  /* red   */
#define LED_2          NRF_GPIO_PIN_MAP(0, 30)  /* green */
#define LED_3          NRF_GPIO_PIN_MAP(0, 6)   /* blue  */
#define LED_START      LED_1
#define LED_STOP       LED_3

#define LEDS_ACTIVE_STATE 0                      /* active low */
#define LEDS_LIST { LED_1, LED_2, LED_3 }
#define LEDS_INV_MASK 0

#define BSP_LED_0      LED_1
#define BSP_LED_1      LED_2
#define BSP_LED_2      LED_3

#define BUTTONS_NUMBER 0
#define BUTTONS_LIST   {}
#define BUTTONS_ACTIVE_STATE 0

#define RX_PIN_NUMBER  NRF_GPIO_PIN_MAP(1, 12)
#define TX_PIN_NUMBER  NRF_GPIO_PIN_MAP(1, 11)
#define CTS_PIN_NUMBER 0xFFFFFFFF
#define RTS_PIN_NUMBER 0xFFFFFFFF
#define HWFC           false
