/* Host tests for core/connect_backoff.c */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "connect_backoff.h"

#define A (const uint8_t *)"\x01\x02\x03\x04\x05\xAA"
#define B (const uint8_t *)"\x01\x02\x03\x04\x05\xBB"
#define C (const uint8_t *)"\x01\x02\x03\x04\x05\xCC"
#define D (const uint8_t *)"\x01\x02\x03\x04\x05\xDD"
#define E (const uint8_t *)"\x01\x02\x03\x04\x05\xEE"

/* A fresh table blocks nothing. */
static void test_empty_blocks_nothing(void)
{
    connect_backoff_t b;
    connect_backoff_reset(&b);
    assert(!connect_backoff_blocks(&b, A, 0));
    assert(!connect_backoff_blocks(&b, A, 999999));
    assert(connect_backoff_count(&b, A) == 0);
}

/* 1, 2, 4, 8, 16, 30, 30 … seconds, saturating at MAX. */
static void test_escalation_schedule(void)
{
    assert(connect_backoff_ms_for_count(0) == 0);
    assert(connect_backoff_ms_for_count(1) == 1000);
    assert(connect_backoff_ms_for_count(2) == 2000);
    assert(connect_backoff_ms_for_count(3) == 4000);
    assert(connect_backoff_ms_for_count(4) == 8000);
    assert(connect_backoff_ms_for_count(5) == 16000);
    assert(connect_backoff_ms_for_count(6) == CONNECT_BACKOFF_MAX_MS);
    assert(connect_backoff_ms_for_count(7) == CONNECT_BACKOFF_MAX_MS);
    assert(connect_backoff_ms_for_count(255) == CONNECT_BACKOFF_MAX_MS);
}

/* One failure blocks for exactly its window, then stops blocking. */
static void test_single_address_window(void)
{
    connect_backoff_t b;
    connect_backoff_reset(&b);

    assert(connect_backoff_note_failure(&b, A, 1000) == 1000);
    assert(connect_backoff_blocks(&b, A, 1000));
    assert(connect_backoff_blocks(&b, A, 1999));
    assert(!connect_backoff_blocks(&b, A, 2000));   /* window is exclusive */
    assert(!connect_backoff_blocks(&b, A, 50000));

    /* A different address is unaffected. */
    assert(!connect_backoff_blocks(&b, B, 1000));
}

/* Expiry must NOT reset the count — the next failure escalates. */
static void test_expiry_preserves_history(void)
{
    connect_backoff_t b;
    connect_backoff_reset(&b);

    connect_backoff_note_failure(&b, A, 0);
    assert(connect_backoff_count(&b, A) == 1);
    assert(!connect_backoff_blocks(&b, A, 10000));      /* expired */
    assert(connect_backoff_count(&b, A) == 1);          /* but remembered */

    assert(connect_backoff_note_failure(&b, A, 10000) == 2000);
    assert(connect_backoff_count(&b, A) == 2);
}

/* THE REGRESSION: alternating failures must each keep their own history.
 * With a single tracked address this thrashed — every failure looked like the
 * first one and nothing was ever blocked. */
static void test_alternating_addresses_keep_history(void)
{
    connect_backoff_t b;
    connect_backoff_reset(&b);

    assert(connect_backoff_note_failure(&b, A, 0) == 1000);
    assert(connect_backoff_note_failure(&b, B, 100) == 1000);
    /* A must still be tracked, and escalate rather than restart at 1. */
    assert(connect_backoff_note_failure(&b, A, 200) == 2000);
    assert(connect_backoff_count(&b, A) == 2);
    assert(connect_backoff_count(&b, B) == 1);

    /* And A is actually blocked, which is the whole point. */
    assert(connect_backoff_blocks(&b, A, 300));

    /* Keep alternating: both escalate independently. */
    connect_backoff_note_failure(&b, B, 400);
    connect_backoff_note_failure(&b, A, 500);
    assert(connect_backoff_count(&b, A) == 3);
    assert(connect_backoff_count(&b, B) == 2);
}

/* Success forgives only the address that succeeded. */
static void test_success_is_per_address(void)
{
    connect_backoff_t b;
    connect_backoff_reset(&b);

    connect_backoff_note_failure(&b, A, 0);
    connect_backoff_note_failure(&b, A, 0);
    connect_backoff_note_failure(&b, B, 0);
    assert(connect_backoff_count(&b, A) == 2);

    connect_backoff_note_success(&b, A);
    assert(connect_backoff_count(&b, A) == 0);
    assert(!connect_backoff_blocks(&b, A, 0));

    /* B keeps its cooldown — a good machine must not forgive a bad one. */
    assert(connect_backoff_count(&b, B) == 1);
    assert(connect_backoff_blocks(&b, B, 0));

    /* Success on an untracked address is harmless. */
    connect_backoff_note_success(&b, C);
    assert(connect_backoff_count(&b, B) == 1);
}

/* More failing addresses than slots: evict the least-recently-failed, and keep
 * tracking the newcomer. */
static void test_eviction_takes_the_stalest(void)
{
    connect_backoff_t b;
    connect_backoff_reset(&b);
    assert(CONNECT_BACKOFF_SLOTS == 4);

    connect_backoff_note_failure(&b, A, 1000);   /* oldest */
    connect_backoff_note_failure(&b, B, 2000);
    connect_backoff_note_failure(&b, C, 3000);
    connect_backoff_note_failure(&b, D, 4000);
    connect_backoff_note_failure(&b, E, 5000);   /* forces an eviction */

    assert(connect_backoff_count(&b, E) == 1);   /* newcomer tracked */
    assert(connect_backoff_count(&b, A) == 0);   /* stalest evicted */
    assert(connect_backoff_count(&b, B) == 1);   /* the rest survive */
    assert(connect_backoff_count(&b, C) == 1);
    assert(connect_backoff_count(&b, D) == 1);
}

/* A freed slot is reused rather than forcing an eviction. */
static void test_success_frees_a_slot(void)
{
    connect_backoff_t b;
    connect_backoff_reset(&b);

    connect_backoff_note_failure(&b, A, 1000);
    connect_backoff_note_failure(&b, B, 2000);
    connect_backoff_note_failure(&b, C, 3000);
    connect_backoff_note_failure(&b, D, 4000);
    connect_backoff_note_success(&b, B);         /* frees B's slot */
    connect_backoff_note_failure(&b, E, 5000);

    assert(connect_backoff_count(&b, E) == 1);
    assert(connect_backoff_count(&b, A) == 1);   /* nothing evicted */
    assert(connect_backoff_count(&b, C) == 1);
    assert(connect_backoff_count(&b, D) == 1);
}

/* The millisecond counter wraps at 2^32; unsigned subtraction must ride it. */
static void test_clock_wrap(void)
{
    connect_backoff_t b;
    connect_backoff_reset(&b);

    uint32_t before_wrap = 0xFFFFFF00u;
    connect_backoff_note_failure(&b, A, before_wrap);   /* 1000 ms window */

    assert(connect_backoff_blocks(&b, A, before_wrap));  /* 0 ms later */
    assert(connect_backoff_blocks(&b, A, 0x00000000u));  /* 256 ms later */
    assert(connect_backoff_blocks(&b, A, 0x000002E7u));  /* 999 ms later */
    assert(!connect_backoff_blocks(&b, A, 0x000002E8u)); /* 1000 ms later */
}

/* The count saturates instead of wrapping to 0 (which would reset the delay). */
static void test_count_saturates(void)
{
    connect_backoff_t b;
    connect_backoff_reset(&b);

    for (int i = 0; i < 300; i++) {
        connect_backoff_note_failure(&b, A, (uint32_t)i * 60000u);
    }
    assert(connect_backoff_count(&b, A) == 255);
    assert(connect_backoff_ms_for_count(connect_backoff_count(&b, A))
           == CONNECT_BACKOFF_MAX_MS);
}

int main(void)
{
    test_empty_blocks_nothing();
    test_escalation_schedule();
    test_single_address_window();
    test_expiry_preserves_history();
    test_alternating_addresses_keep_history();
    test_success_is_per_address();
    test_eviction_takes_the_stalest();
    test_success_frees_a_slot();
    test_clock_wrap();
    test_count_saturates();
    printf("connect_backoff: OK\n");
    return 0;
}
