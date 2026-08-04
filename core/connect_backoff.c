#include "connect_backoff.h"

#include <string.h>

/* Index of addr's entry, or -1. count == 0 means the slot is unused, so a
 * cleared slot never matches even though its address bytes may linger. */
static int find(const connect_backoff_t *b, const uint8_t *addr)
{
    for (int i = 0; i < CONNECT_BACKOFF_SLOTS; i++) {
        if (b->slots[i].count != 0 &&
            memcmp(b->slots[i].addr, addr, CONNECT_BACKOFF_ADDR_LEN) == 0) {
            return i;
        }
    }
    return -1;
}

void connect_backoff_reset(connect_backoff_t *b)
{
    memset(b, 0, sizeof *b);
}

uint32_t connect_backoff_ms_for_count(uint8_t count)
{
    if (count == 0) return 0;

    uint32_t ms = CONNECT_BACKOFF_BASE_MS;
    for (uint8_t i = 1; i < count; i++) {
        /* Check before doubling so the multiply cannot overshoot MAX. */
        if (ms >= CONNECT_BACKOFF_MAX_MS / 2) return CONNECT_BACKOFF_MAX_MS;
        ms *= 2;
    }
    return ms;
}

bool connect_backoff_blocks(const connect_backoff_t *b, const uint8_t *addr,
                            uint32_t now_ms)
{
    int i = find(b, addr);
    if (i < 0) return false;

    /* Unsigned subtraction, so a now_ms that has wrapped past 2^32 still gives
     * the true elapsed time. */
    uint32_t elapsed = now_ms - b->slots[i].last_fail_ms;
    return elapsed < connect_backoff_ms_for_count(b->slots[i].count);
}

uint32_t connect_backoff_note_failure(connect_backoff_t *b, const uint8_t *addr,
                                      uint32_t now_ms)
{
    int i = find(b, addr);

    if (i < 0) {
        for (int k = 0; k < CONNECT_BACKOFF_SLOTS; k++) {
            if (b->slots[k].count == 0) { i = k; break; }
        }
    }

    if (i < 0) {
        /* Full: evict the least-recently-failed entry. Its cooldown is closest
         * to expiring, so it is the one whose history is worth least. Same
         * wrap-safe subtraction as above. */
        i = 0;
        for (int k = 1; k < CONNECT_BACKOFF_SLOTS; k++) {
            if ((uint32_t)(now_ms - b->slots[k].last_fail_ms) >
                (uint32_t)(now_ms - b->slots[i].last_fail_ms)) {
                i = k;
            }
        }
        b->slots[i].count = 0;
    }

    connect_backoff_entry_t *e = &b->slots[i];
    if (e->count == 0) {
        memcpy(e->addr, addr, CONNECT_BACKOFF_ADDR_LEN);
        e->count = 1;
    } else if (e->count < 255) {
        /* Saturate rather than wrap: a count that rolled to 0 would read as
         * "no history" and hand a persistently bad machine a clean slate. */
        e->count++;
    }
    e->last_fail_ms = now_ms;

    return connect_backoff_ms_for_count(e->count);
}

void connect_backoff_note_success(connect_backoff_t *b, const uint8_t *addr)
{
    int i = find(b, addr);
    if (i >= 0) b->slots[i].count = 0;
}

uint8_t connect_backoff_count(const connect_backoff_t *b, const uint8_t *addr)
{
    int i = find(b, addr);
    return i < 0 ? 0 : b->slots[i].count;
}
