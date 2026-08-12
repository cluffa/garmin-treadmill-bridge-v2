#include "ant_request.h"

/* Page 70 wire layout, from the SDK's own encoder/decoder
 * ($SDK_ROOT/components/ant/ant_profiles/ant_common/pages/ant_common_page_70.c,
 * ant_common_page70_data_layout_t laid over payload bytes 1..7):
 *
 *   byte 0  page number, 70
 *   byte 1  reserved (0xFF)
 *   byte 2  reserved (0xFF)
 *   byte 3  descriptor, LSB   } ignored: it selects a sub-page for pages that
 *   byte 4  descriptor, MSB   } have one, and none of ours do
 *   byte 5  requested transmission response
 *   byte 6  requested page number
 *   byte 7  command type
 */
#define P70_RESPONSE   5
#define P70_PAGE       6
#define P70_COMMAND    7

#define P70_CMD_DATA_PAGE   0x01   /* ant_page70_command_t; 0x02 is ANT-FS */

/* Requested transmission response byte: bit 7 asks for acknowledged messages,
 * bits 0-6 are the transmission count. 0x00 is the profile's "invalid" value
 * and 0x80 means "transmit until acknowledged" (count field unused). */
#define P70_RESP_ACK        0x80
#define P70_RESP_COUNT      0x7F
#define P70_RESP_INVALID    0x00

void ant_request_init(ant_request_t *r)
{
    r->page      = 0;
    r->remaining = 0;
    r->ack       = false;
    r->until_ack = false;
    r->hold      = 0;
    r->requests  = 0;
    r->rejects   = 0;
    r->last_page = 0;
}

bool ant_request_page_supported(uint8_t page)
{
    return page == 1 || page == 2 || page == 22 || page == 80 || page == 81;
}

bool ant_request_on_rx(ant_request_t *r, const uint8_t *msg, uint8_t len)
{
    /* Not a page 70 — the watch's own traffic, or a short frame. Silent: see
     * the header on why this must not touch the counters. */
    if (len < 8 || msg == 0 || msg[0] != ANT_COMMON_PAGE_70) {
        return false;
    }

    uint8_t resp = msg[P70_RESPONSE];
    uint8_t page = msg[P70_PAGE];

    /* Understood, but nothing to do about it. Counted so a non-zero `rejects`
     * with a zero `requests` reads as "the watch asks for something we don't
     * have" rather than as silence. */
    if (msg[P70_COMMAND] != P70_CMD_DATA_PAGE ||
        resp == P70_RESP_INVALID ||
        !ant_request_page_supported(page)) {
        r->rejects++;
        return false;
    }

    uint8_t count = resp & P70_RESP_COUNT;

    r->page      = page;
    r->ack       = (resp & P70_RESP_ACK) != 0;
    r->until_ack = (resp == P70_RESP_ACK);   /* exactly 0x80, no count */

    if (r->until_ack) {
        r->remaining = ANT_REQUEST_MAX_UNTIL_ACK;
    } else {
        r->remaining = count > ANT_REQUEST_MAX_COUNT ? ANT_REQUEST_MAX_COUNT
                                                     : count;
    }

    /* Answer on the very next slot. A request satisfied 16 s later is worth no
     * more than the unsolicited rotation this module exists to supplement. */
    r->hold = 0;

    r->requests++;
    r->last_page = page;
    return true;
}

bool ant_request_take_slot(ant_request_t *r, uint8_t *page, bool *ack)
{
    if (r->remaining == 0) {
        return false;
    }
    if (r->hold > 0) {
        r->hold--;
        return false;
    }

    *page = r->page;
    *ack  = r->ack;

    r->remaining--;
    r->hold = ANT_REQUEST_SPREAD - 1;
    return true;
}

void ant_request_on_response_ack(ant_request_t *r, bool ok)
{
    if (ok && r->until_ack) {
        r->remaining = 0;
    }
}

void ant_request_retry_slot(ant_request_t *r)
{
    /* Saturate rather than wrap: a stuck radio must not roll the counter over
     * into a fresh burst. */
    if (r->remaining < 0xFF) {
        r->remaining++;
    }
    r->hold = 0;   /* the next slot is eligible; nothing went out on this one */
}
