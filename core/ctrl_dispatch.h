#pragma once

#include <stdint.h>

/* Callback used by ctrl_dispatch() to send a response line.
 * The implementation must copy `msg` if it needs it after returning. */
typedef void (*ctrl_tx_fn)(const char *msg, void *ctx);

/* Bytes of console output lost WHILE A CONSOLE WAS ATTACHED, cumulative since
 * boot, reported by STATUS as "tx_drops". Output emitted with no console
 * attached is drained by design and deliberately not counted (on USB-CDC the
 * stack refuses every write while DTR is clear, so "before you attached" is
 * always lost in full — counting it would drown the signal). Deliberately not
 * reset when the port re-opens: an earlier version did, which zeroed the
 * history at the only moment it became readable.
 *
 * A console that silently loses output is worse than one that is merely slow:
 * a missing log line reads as "that code path did not run". This makes the loss
 * visible instead. Non-zero means the FIFO overflowed or the USB stack refused
 * a write during some attached session, so treat surrounding logs as
 * incomplete.
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
