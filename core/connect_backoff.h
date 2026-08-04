#pragma once
/*
 * connect_backoff.h — per-address cooldown for treadmills that accept a
 * connection but never become usable.
 *
 * A machine that fails to become usable must not be retried immediately. Two
 * different failures produce the same tight loop: HCI 0x3E
 * (CONN_FAILED_TO_BE_ESTABLISHED), and a GATT discovery failure. Either way:
 * connect -> fail -> disconnect -> scan -> the same device wins again ->
 * connect, with no pause. That keeps the radio busy enough to starve the watch
 * link into a supervision timeout, so one unusable treadmill in range takes
 * down the link that actually matters.
 *
 * WHY A TABLE AND NOT ONE SLOT
 * ---------------------------
 * This started as a single tracked address, which silently did nothing
 * whenever more than one machine was misbehaving. note_failure() overwrote the
 * slot on every *different* address, and blocks() only matched the one address
 * it happened to be holding, so with two failing machines A and B:
 *
 *     fail A  -> slot = A, count 1
 *     fail B  -> slot = B, count 1   (A's history destroyed)
 *     fail A  -> slot = A, count 1   (B's history destroyed, A restarted at 1)
 *
 * Nothing was ever blocked and nothing ever escalated. Since the pick is by
 * RSSI and RSSI ordering flips between nearby machines, alternating is the
 * normal case in a room with several treadmills, not a corner case. A table
 * gives each address its own history.
 *
 * TIME
 * ----
 * now_ms is a free-running millisecond counter. Elapsed time is computed with
 * unsigned subtraction, so a counter that wraps at 2^32 is handled correctly
 * as long as no single cooldown spans ~49 days. Callers on nRF5 must convert
 * the 24-bit RTC to such a counter rather than passing raw ticks.
 */

#include <stdbool.h>
#include <stdint.h>

#define CONNECT_BACKOFF_ADDR_LEN 6

/* Enough for a row of machines in one room. Each slot costs 11 bytes; the
 * eviction policy below keeps a bigger table from being necessary. */
#define CONNECT_BACKOFF_SLOTS    4

#define CONNECT_BACKOFF_BASE_MS  1000u
#define CONNECT_BACKOFF_MAX_MS   30000u

typedef struct {
    uint8_t  addr[CONNECT_BACKOFF_ADDR_LEN];
    uint32_t last_fail_ms;
    uint8_t  count;              /* 0 = slot unused */
} connect_backoff_entry_t;

typedef struct {
    connect_backoff_entry_t slots[CONNECT_BACKOFF_SLOTS];
} connect_backoff_t;

/* Forget everything. */
void connect_backoff_reset(connect_backoff_t *b);

/* Cooldown for the n'th consecutive failure: 1, 2, 4, 8, 16, 30, 30 … seconds.
 * count 0 yields 0. Exposed so the caller can log what it just armed.
 *
 * The base step is roughly a no-op by design: the policy tick already runs at
 * 1 Hz, so a 1000 ms cooldown just means "retry on the next tick", which is
 * what you want for a transient failure. The escalation does the real work. */
uint32_t connect_backoff_ms_for_count(uint8_t count);

/* True while addr is still cooling down.
 *
 * Expiry deliberately does NOT clear the entry's count. An earlier version
 * cleared it, which silently defeated the whole escalation: the next failure
 * found no history and restarted at 1, so every retry logged "attempt 1 … 1000
 * ms" no matter how many times in a row the machine had failed. Expiry just
 * stops blocking; only note_success() clears history. */
bool connect_backoff_blocks(const connect_backoff_t *b, const uint8_t *addr,
                            uint32_t now_ms);

/* One attempt on addr never produced a usable link. Escalates its cooldown and
 * returns the new cooldown in ms, for logging.
 *
 * When every slot is in use, the least-recently-failed entry is evicted: its
 * cooldown is the closest to expiring, so it is the one whose loss costs least.
 */
uint32_t connect_backoff_note_failure(connect_backoff_t *b, const uint8_t *addr,
                                      uint32_t now_ms);

/* addr reached a usable link, so its history is forgiven.
 *
 * Only THIS address is cleared. Clearing the whole table would let a machine
 * that works forgive one that does not, and the next disconnect would go
 * straight back to hammering the bad one with no cooldown — the exact loop this
 * module exists to stop. */
void connect_backoff_note_success(connect_backoff_t *b, const uint8_t *addr);

/* Consecutive-failure count for addr, 0 if untracked. For logging and tests. */
uint8_t connect_backoff_count(const connect_backoff_t *b, const uint8_t *addr);
