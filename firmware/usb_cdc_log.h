#pragma once
/*
 * usb_cdc_log.h — USB-CDC ACM console: log backend + interactive ctrl dispatch.
 *
 * Provides:
 *   usb_cdc_log_init()          — init USBD + CDC ACM, start USB
 *   usb_cdc_log_write(const char*) — write a line to the CDC data endpoint
 *   usb_cdc_log_drops()            — bytes dropped since the port opened
 *   usb_cdc_on_line(const char*)   — weak hook: called when a complete line
 *                                    arrives (terminated by \r or \n)
 *
 * The default usb_cdc_on_line() dispatches the line to core/ctrl_dispatch()
 * and writes any reply lines back to CDC.
 *
 * This file also supplies the strong definition of core's weak
 * ctrl_console_drops(), which is what puts usb_cdc_log_drops() into the STATUS
 * reply.
 */

#include <stdint.h>

void usb_cdc_log_init(void);
void usb_cdc_log_write(const char *msg);
void usb_cdc_on_line(const char *line);

/* Console bytes dropped since the last PORT_OPEN (which resets it): the TX FIFO
 * overflowed, or the USB stack refused a write. Non-zero means console output
 * is incomplete — treat any surrounding log as missing lines rather than as
 * evidence a code path did not run. */
uint32_t usb_cdc_log_drops(void);
