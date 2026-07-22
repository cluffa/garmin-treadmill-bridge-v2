#pragma once
/*
 * usb_cdc_log.h — USB-CDC ACM console: log backend + interactive ctrl dispatch.
 *
 * Provides:
 *   usb_cdc_log_init()          — init USBD + CDC ACM, start USB
 *   usb_cdc_log_write(const char*) — write a line to the CDC data endpoint
 *   usb_cdc_on_line(const char*)   — weak hook: called when a complete line
 *                                    arrives (terminated by \r or \n)
 *
 * The default usb_cdc_on_line() dispatches the line to core/ctrl_dispatch()
 * and writes any reply lines back to CDC.
 */

void usb_cdc_log_init(void);
void usb_cdc_log_write(const char *msg);
void usb_cdc_on_line(const char *line);
