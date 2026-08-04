#pragma once

#include <stdint.h>

/* Callback used by ctrl_dispatch() to send a response line.
 * The implementation must copy `msg` if it needs it after returning. */
typedef void (*ctrl_tx_fn)(const char *msg, void *ctx);

/* Bytes of console output the transport has dropped since the port opened,
 * reported by STATUS as "tx_drops".
 *
 * A console that silently loses output is worse than one that is merely slow:
 * a missing log line reads as "that code path did not run". This makes the loss
 * visible instead. Non-zero means the FIFO overflowed or the USB stack refused
 * a write, so treat any surrounding log as incomplete.
 *
 * Weakly defined in core as 0. The platform overrides it — on nRF5 that is
 * firmware/usb_cdc_log.c. Declared here rather than in machine.h on purpose:
 * machine.h is the treadmill facade, and this is a property of the console
 * transport, which core otherwise knows nothing about. */
uint32_t ctrl_console_drops(void);

/* Parse and execute one ASCII command line.
 * `tx` is called zero or more times with JSON response strings (no trailing \n).
 * Thread-safe: individual commands are atomic against concurrent dispatches
 * because the underlying machine_* calls are already serialised. */
void ctrl_dispatch(const char *line, ctrl_tx_fn tx, void *ctx);
