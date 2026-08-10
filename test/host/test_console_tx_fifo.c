/* Host tests for core/console_tx_fifo.c — the USB-CDC console TX FIFO.
 *
 * This logic ran on hardware for a day with its drop counter unexercised: the
 * host drains the port faster than the firmware produces, so overflow never
 * happened and the arithmetic had never once been observed counting. These
 * tests force every drop path the hardware would not produce on demand.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "console_tx_fifo.h"

/* Small power-of-two so wrap and overflow are trivial to reach.
 * Usable capacity is SIZE - 1. */
#define SIZE 16u
#define CAP  (SIZE - 1u)

static console_tx_fifo_t f;
static uint8_t storage[SIZE];

static void fresh(bool open)
{
    console_tx_fifo_init(&f, storage, SIZE);
    console_tx_fifo_port_event(&f, open);
}

/* Bytes in, same bytes out, in order. */
static void test_roundtrip(void)
{
    fresh(true);
    console_tx_fifo_push(&f, (const uint8_t *)"hello", 5);
    assert(console_tx_fifo_pending(&f) == 5);

    uint8_t out[SIZE];
    size_t n = console_tx_fifo_pop(&f, out, sizeof(out));
    assert(n == 5);
    assert(memcmp(out, "hello", 5) == 0);
    assert(console_tx_fifo_pending(&f) == 0);
    assert(console_tx_fifo_drops(&f) == 0);
}

/* pop honours max and leaves the remainder queued, still in order —
 * this is the one-packet-per-TX_DONE staging path. */
static void test_pop_respects_max(void)
{
    fresh(true);
    console_tx_fifo_push(&f, (const uint8_t *)"abcdefghij", 10);

    uint8_t out[4];
    assert(console_tx_fifo_pop(&f, out, 4) == 4);
    assert(memcmp(out, "abcd", 4) == 0);
    assert(console_tx_fifo_pending(&f) == 6);
    assert(console_tx_fifo_pop(&f, out, 4) == 4);
    assert(memcmp(out, "efgh", 4) == 0);
    uint8_t rest[4];
    assert(console_tx_fifo_pop(&f, rest, 4) == 2);
    assert(memcmp(rest, "ij", 2) == 0);
}

/* Order survives index wrap-around. 3-byte messages through a 16-slot ring
 * hit every alignment of head/tail against the mask. */
static void test_wraparound_order(void)
{
    fresh(true);
    for (int i = 0; i < 10000; i++) {
        uint8_t in[3] = { (uint8_t)i, (uint8_t)(i >> 8), (uint8_t)(i * 7) };
        console_tx_fifo_push(&f, in, 3);
        uint8_t out[3];
        assert(console_tx_fifo_pop(&f, out, 3) == 3);
        assert(memcmp(in, out, 3) == 0);
    }
    assert(console_tx_fifo_drops(&f) == 0);
}

/* Exactly CAP bytes fit; the CAP+1'th is the first drop. */
static void test_capacity_is_size_minus_one(void)
{
    fresh(true);
    uint8_t block[SIZE];
    memset(block, 'x', sizeof(block));

    console_tx_fifo_push(&f, block, CAP);
    assert(console_tx_fifo_pending(&f) == CAP);
    assert(console_tx_fifo_drops(&f) == 0);

    console_tx_fifo_push(&f, block, 1);
    assert(console_tx_fifo_pending(&f) == CAP);
    assert(console_tx_fifo_drops(&f) == 1);
}

/* A message that half-fits drops exactly its tail: free space 5, message 9,
 * drops must be 4 — not 9, not "the whole message". */
static void test_partial_overflow_counts_exact_tail(void)
{
    fresh(true);
    uint8_t block[32];
    memset(block, 'y', sizeof(block));

    console_tx_fifo_push(&f, block, CAP - 5);   /* leave 5 free */
    console_tx_fifo_push(&f, block, 9);
    assert(console_tx_fifo_pending(&f) == CAP);
    assert(console_tx_fifo_drops(&f) == 4);

    /* Every later byte of a full FIFO is counted too. */
    console_tx_fifo_push(&f, block, 32);
    assert(console_tx_fifo_drops(&f) == 4 + 32);
}

/* Overflow with the port CLOSED is the by-design drain — never counted. */
static void test_closed_port_overflow_uncounted(void)
{
    fresh(false);
    uint8_t block[64];
    memset(block, 'z', sizeof(block));
    console_tx_fifo_push(&f, block, 64);        /* way past capacity */
    assert(console_tx_fifo_pending(&f) == CAP);
    assert(console_tx_fifo_drops(&f) == 0);
}

/* A refused write counts iff the port is open. */
static void test_refused_write_accounting(void)
{
    fresh(true);
    console_tx_fifo_note_refused(&f, 17);
    assert(console_tx_fifo_drops(&f) == 17);

    console_tx_fifo_port_event(&f, false);
    console_tx_fifo_note_refused(&f, 40);       /* nobody listening — by design */
    assert(console_tx_fifo_drops(&f) == 17);
}

/* Re-opening the port discards stale queued bytes (uncounted) but PRESERVES
 * the drop history — the regression this module exists to fix: resetting the
 * counter on open zeroed it at the only moment it became readable. */
static void test_reopen_preserves_drops_discards_bytes(void)
{
    fresh(true);
    uint8_t block[SIZE];
    memset(block, 'q', sizeof(block));
    console_tx_fifo_push(&f, block, SIZE);      /* CAP stored, 1 counted */
    assert(console_tx_fifo_drops(&f) == 1);

    console_tx_fifo_port_event(&f, false);
    console_tx_fifo_push(&f, block, 4);         /* queued while closed */

    console_tx_fifo_port_event(&f, true);
    assert(console_tx_fifo_pending(&f) == 0);   /* stale bytes discarded */
    assert(console_tx_fifo_drops(&f) == 1);     /* history intact */
}

/* The end-to-end shape of the real failure: a burst bigger than the FIFO
 * arrives while the (open) port cannot drain. Conservation must hold:
 * delivered + dropped == offered. */
static void test_burst_conservation(void)
{
    fresh(true);
    uint32_t offered = 0, delivered = 0;
    uint8_t out[8];
    size_t n;

    for (int i = 0; i < 100; i++) {
        uint8_t line[24];
        memset(line, (uint8_t)('A' + (i % 26)), sizeof(line));
        console_tx_fifo_push(&f, line, sizeof(line));
        offered += sizeof(line);
        /* one slow 8-byte drain per 24-byte push — guaranteed overflow */
        delivered += (uint32_t)console_tx_fifo_pop(&f, out, sizeof(out));
    }
    while ((n = console_tx_fifo_pop(&f, out, sizeof(out))) > 0) {
        delivered += (uint32_t)n;
    }
    assert(console_tx_fifo_pending(&f) == 0);
    assert(console_tx_fifo_drops(&f) > 0);
    /* The whole point of the counter: no byte unaccounted for. */
    assert(delivered + console_tx_fifo_drops(&f) == offered);
}

int main(void)
{
    test_roundtrip();
    test_pop_respects_max();
    test_wraparound_order();
    test_capacity_is_size_minus_one();
    test_partial_overflow_counts_exact_tail();
    test_closed_port_overflow_uncounted();
    test_refused_write_accounting();
    test_reopen_preserves_drops_discards_bytes();
    test_burst_conservation();
    printf("test_console_tx_fifo: all tests passed\n");
    return 0;
}
