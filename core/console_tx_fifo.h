#pragma once
/*
 * console_tx_fifo.h — byte FIFO for a console transport that may drop output,
 * with an honest account of what was lost.
 *
 * Extracted from firmware/usb_cdc_log.c so the drop accounting can be host-
 * tested — the counter exists to make silent console loss visible, so its own
 * arithmetic being unexercised defeated the point (the FIFO on hardware never
 * overflows in normal use, because the host drains faster than we produce).
 *
 * WHAT COUNTS AS A DROP
 * ---------------------
 * Output emitted while no console is attached is discarded *by design* — the
 * transport must drain rather than wedge, and on USB-CDC every write is refused
 * while DTR is clear, so "everything before you attached" is always lost. A
 * counter that included those bytes would read as a huge number that means
 * nothing. So drops are counted ONLY while the port is open: a non-zero value
 * means output was lost while someone was (or had been) attached to see it.
 *
 * The counter is cumulative since boot and is deliberately NOT reset when the
 * port re-opens. An earlier version reset it on PORT_OPEN, which zeroed the
 * history at the only moment it became readable — reconnecting to *ask* about
 * drops destroyed the answer.
 *
 * LOCKING
 * -------
 * None here. The caller serialises access (on nRF5, critical regions around
 * every call — pushes happen from both IRQ and thread context). The head/tail
 * are plain uint16_t; `size` must be a power of two; usable capacity is
 * size - 1 (one slot distinguishes full from empty).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t  *buf;
    uint16_t  size;       /* power of two */
    uint16_t  head;       /* append position */
    uint16_t  tail;       /* drain position  */
    bool      port_open;  /* is anyone listening? gates drop accounting */
    uint32_t  drops;      /* bytes lost while port_open, cumulative since init */
} console_tx_fifo_t;

/* buf/size: caller-owned storage; size MUST be a power of two. */
void console_tx_fifo_init(console_tx_fifo_t *f, uint8_t *buf, uint16_t size);

/* Append len bytes. When the FIFO fills mid-message the tail of the message is
 * discarded; the discarded byte count is added to drops iff the port is open. */
void console_tx_fifo_push(console_tx_fifo_t *f, const uint8_t *data, size_t len);

/* Drain up to max bytes into dst, returning how many were staged. The bytes
 * are consumed: if the transport then refuses the write, report it with
 * console_tx_fifo_note_refused() — deliberately no un-pop, so a console nobody
 * is reading drains instead of wedging every later message behind it. */
size_t console_tx_fifo_pop(console_tx_fifo_t *f, uint8_t *dst, size_t max);

/* The transport refused a write of len already-popped bytes. Counted as drops
 * iff the port is open (refusals while closed are the by-design drain). */
void console_tx_fifo_note_refused(console_tx_fifo_t *f, size_t len);

/* Port open/close transitions. Open discards whatever is queued (it is stale
 * pre-attach output, uncounted by the rule above) but PRESERVES the drop
 * counter. */
void console_tx_fifo_port_event(console_tx_fifo_t *f, bool open);

/* Bytes queued and not yet drained. */
uint16_t console_tx_fifo_pending(const console_tx_fifo_t *f);

uint32_t console_tx_fifo_drops(const console_tx_fifo_t *f);
