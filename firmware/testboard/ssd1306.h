#pragma once
/*
 * ssd1306.h — SSD1306 128x64 monochrome OLED via I2C (addr 0x3C).
 *
 * I2C pins: SDA = P0.04 (D4), SCL = P0.05 (D5) — Seeed XIAO expansion board.
 *
 * Provides a simple framebuffer + 5x7 font text renderer:
 *   - 21 characters per line (6 px wide with 1 px left pad)
 *   - 8 lines (8 px high on 8-page SSD1306)
 *
 * M2 Task 2.3.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Initialise I2C and OLED ------------------------------------------------ */
void ssd1306_init(void);

/* ---- Clear the framebuffer -------------------------------------------------- */
void ssd1306_clear(void);

/* ---- Render a NUL-terminated string at a character column/row ---------------
 *
 * col 0..20  (0 = leftmost; each char column is 6 px wide)
 * row 0..7   (0 = topmost; each row is one 8-px-tall page)
 *
 * Characters outside the displayable ASCII range 0x20..0x7E are rendered
 * as spaces.  No line-wrap: text that overflows the right edge is clipped.
 */
void ssd1306_text(uint8_t col, uint8_t row, const char *text);

/* ---- Send the framebuffer to the OLED -------------------------------------- */
void ssd1306_show(void);

#ifdef __cplusplus
}
#endif
