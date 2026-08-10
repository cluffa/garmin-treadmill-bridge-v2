#pragma once
/*
 * usb_cdc_log.h — USB-CDC ACM console: log backend + interactive ctrl dispatch.
 *
 * Provides:
 *   usb_cdc_log_init()          — init USBD + CDC ACM, start USB
 *   usb_cdc_log_write(const char*) — write a line to the CDC data endpoint
 *   usb_cdc_log_drops()            — bytes lost while a console was attached
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

/* Console bytes lost while the port was OPEN, cumulative since boot: the TX
 * FIFO overflowed, or the USB stack refused a write mid-session. Output
 * emitted with no console attached is drained by design and not counted, and
 * the counter deliberately survives PORT_OPEN — resetting it there zeroed the
 * history at the only moment it became readable. Non-zero means console output
 * was incomplete during some attached session — treat surrounding logs as
 * missing lines rather than as evidence a code path did not run.
 * Accounting lives in core/console_tx_fifo.c and is host-tested. */
uint32_t usb_cdc_log_drops(void);
