#pragma once

/* Callback used by ctrl_dispatch() to send a response line.
 * The implementation must copy `msg` if it needs it after returning. */
typedef void (*ctrl_tx_fn)(const char *msg, void *ctx);

/* Parse and execute one ASCII command line.
 * `tx` is called zero or more times with JSON response strings (no trailing \n).
 * Thread-safe: individual commands are atomic against concurrent dispatches
 * because the underlying machine_* calls are already serialised. */
void ctrl_dispatch(const char *line, ctrl_tx_fn tx, void *ctx);
