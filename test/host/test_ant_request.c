#include "ant_request.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Build an ANT+ common page 70 the way a display sends it. Layout is from
 * $SDK_ROOT/components/ant/ant_profiles/ant_common/pages/ant_common_page_70.c:
 * two reserved bytes, a 16-bit descriptor, the requested transmission
 * response, the requested page number, then the command type. */
static void mk70(uint8_t out[8], uint8_t page, uint8_t resp, uint8_t cmd)
{
    out[0] = 70;
    out[1] = 0xFF;  out[2] = 0xFF;          /* reserved */
    out[3] = 0xFF;  out[4] = 0xFF;          /* descriptor: invalid */
    out[5] = resp;
    out[6] = page;
    out[7] = cmd;
}

#define REQ_DATA_PAGE 0x01                  /* ant_page70_command_t */
#define REQ_ANT_FS    0x02

/* Run one TX slot; returns the page sent, or 0 for "the rotation keeps it". */
static uint8_t slot(ant_request_t *r, bool *ack)
{
    uint8_t page = 0;
    bool    a    = false;
    if (!ant_request_take_slot(r, &page, &a)) return 0;
    if (ack) *ack = a;
    return page;
}

int main(void)
{
    ant_request_t r;
    uint8_t       m[8];
    bool          ack;

    /* ---- which pages we can answer at all ------------------------------- *
     * Only the pages the footpod actually builds. The profile says to ignore
     * a request for anything else rather than answer it with something. */
    assert(ant_request_page_supported(1));
    assert(ant_request_page_supported(2));
    assert(ant_request_page_supported(22));
    assert(ant_request_page_supported(80));
    assert(ant_request_page_supported(81));
    assert(!ant_request_page_supported(0));
    assert(!ant_request_page_supported(3));
    assert(!ant_request_page_supported(16));
    assert(!ant_request_page_supported(70));   /* we never answer with a request */
    assert(!ant_request_page_supported(255));

    /* ---- a fresh controller owes nothing -------------------------------- */
    ant_request_init(&r);
    assert(r.requests == 0 && r.rejects == 0 && r.last_page == 0);
    for (int i = 0; i < 16; i++) assert(slot(&r, NULL) == 0);

    /* ---- anything that is not a page 70 is not ours ---------------------- *
     * The watch's own broadcasts land on this channel too. They must not
     * count as rejected requests, or the STATUS counter stops answering the
     * question it exists for ("did the watch ever ask?"). */
    mk70(m, 22, 4, REQ_DATA_PAGE);
    m[0] = 22;
    assert(!ant_request_on_rx(&r, m, 8));
    assert(r.requests == 0 && r.rejects == 0);

    /* ---- a page 70 we cannot act on is counted, not obeyed --------------- */
    mk70(m, 22, 4, REQ_ANT_FS);              /* ANT-FS session, not a page */
    assert(!ant_request_on_rx(&r, m, 8));
    assert(r.requests == 0 && r.rejects == 1);

    mk70(m, 22, 0x00, REQ_DATA_PAGE);        /* invalid response: no transmits */
    assert(!ant_request_on_rx(&r, m, 8));
    assert(r.rejects == 2);

    mk70(m, 3, 4, REQ_DATA_PAGE);            /* a page we do not have */
    assert(!ant_request_on_rx(&r, m, 8));
    assert(r.rejects == 3);

    mk70(m, 22, 4, REQ_DATA_PAGE);           /* short message: not a page 70 */
    assert(!ant_request_on_rx(&r, m, 7));
    assert(r.requests == 0 && r.rejects == 3);
    for (int i = 0; i < 16; i++) assert(slot(&r, NULL) == 0);

    /* ---- broadcast request for page 22, four transmissions --------------- *
     * The case the whole module exists for: the watch asks for capabilities.
     * The first response goes out on the very next slot — a request answered
     * 16 s later is no better than the unsolicited rotation we already had. */
    ant_request_init(&r);
    mk70(m, 22, 4, REQ_DATA_PAGE);
    assert(ant_request_on_rx(&r, m, 8));
    assert(r.requests == 1 && r.last_page == 22 && r.rejects == 0);

    ack = true;
    assert(slot(&r, &ack) == 22);
    assert(!ack);                            /* bit 7 clear: plain broadcast */

    /* ---- responses are spread, never adjacent --------------------------- *
     * firmware/ant_sdm.c's interleave keeps at least three page 1s inside any
     * one-second window (four TX slots) so the watch never records a second
     * with no speed. A burst of responses would blow that if they went out
     * back to back, so at most one lands per four slots: with the rotation's
     * own background page that still leaves two page 1s. */
    int sent = 1, gap = 0;
    for (int i = 0; i < 64; i++) {
        if (slot(&r, NULL) == 22) {
            assert(gap >= ANT_REQUEST_SPREAD - 1);
            sent++;
            gap = 0;
        } else {
            gap++;
        }
    }
    assert(sent == 4);                       /* exactly what was asked for */
    for (int i = 0; i < 16; i++) assert(slot(&r, NULL) == 0);   /* then idle */

    /* ---- acknowledged-until-success (0x80) ------------------------------- */
    ant_request_init(&r);
    mk70(m, 22, 0x80, REQ_DATA_PAGE);
    assert(ant_request_on_rx(&r, m, 8));
    assert(slot(&r, &ack) == 22);
    assert(ack);                             /* must go out acknowledged */

    /* a failed transfer is retried on the next eligible slot */
    ant_request_on_response_ack(&r, false);
    for (int i = 0; i < ANT_REQUEST_SPREAD - 1; i++) assert(slot(&r, NULL) == 0);
    assert(slot(&r, NULL) == 22);

    /* ...and a successful one ends it immediately */
    ant_request_on_response_ack(&r, true);
    for (int i = 0; i < 32; i++) assert(slot(&r, NULL) == 0);

    /* ---- a requester that never acknowledges cannot hold us forever ------ *
     * The SDK does not cap this; we do, because the degraded interleave above
     * is only acceptable as a burst. Deliberate divergence. */
    ant_request_init(&r);
    mk70(m, 22, 0x80, REQ_DATA_PAGE);
    assert(ant_request_on_rx(&r, m, 8));
    sent = 0;
    for (int i = 0; i < 4 * ANT_REQUEST_MAX_UNTIL_ACK + 64; i++) {
        if (slot(&r, NULL) == 22) {
            sent++;
            ant_request_on_response_ack(&r, false);
        }
    }
    assert(sent == ANT_REQUEST_MAX_UNTIL_ACK);

    /* ---- and neither can an absurd transmit count ------------------------ */
    ant_request_init(&r);
    mk70(m, 22, 0x7F, REQ_DATA_PAGE);        /* 127 transmissions */
    assert(ant_request_on_rx(&r, m, 8));
    sent = 0;
    for (int i = 0; i < 4 * 127 + 64; i++) {
        if (slot(&r, NULL) == 22) sent++;
    }
    assert(sent == ANT_REQUEST_MAX_COUNT);

    /* ---- acknowledged with a count is not until-success ------------------ *
     * Bit 7 set with a non-zero count means "send this many, acknowledged".
     * One successful transfer does not discharge the rest, matching the SDK,
     * which only goes idle on completion in the until-success state. */
    ant_request_init(&r);
    mk70(m, 22, 0x83, REQ_DATA_PAGE);        /* ack + 3 transmissions */
    assert(ant_request_on_rx(&r, m, 8));
    assert(slot(&r, &ack) == 22 && ack);
    ant_request_on_response_ack(&r, true);
    sent = 1;
    for (int i = 0; i < 64; i++) if (slot(&r, NULL) == 22) sent++;
    assert(sent == 3);

    /* ---- a message the radio refused is still owed ----------------------- *
     * sd_ant_*_message_tx() can refuse outright, which is not a failed
     * transfer: nothing reached the air, so the transmission is still owed and
     * the next slot is eligible immediately. */
    ant_request_init(&r);
    mk70(m, 22, 2, REQ_DATA_PAGE);
    assert(ant_request_on_rx(&r, m, 8));
    assert(slot(&r, NULL) == 22);
    ant_request_retry_slot(&r);
    assert(slot(&r, NULL) == 22);             /* immediately, not after a hold */
    sent = 1;
    for (int i = 0; i < 64; i++) if (slot(&r, NULL) == 22) sent++;
    assert(sent == 2);                        /* still exactly two on the air */

    /* ---- a page other than 22 is answered too --------------------------- *
     * A display that asks for 80/81 to identify the sensor gets them; there
     * is no reason to special-case capabilities. */
    ant_request_init(&r);
    mk70(m, 80, 2, REQ_DATA_PAGE);
    assert(ant_request_on_rx(&r, m, 8));
    assert(r.last_page == 80);
    assert(slot(&r, NULL) == 80);

    /* ---- a new request replaces one still in flight ---------------------- */
    mk70(m, 81, 3, REQ_DATA_PAGE);
    assert(ant_request_on_rx(&r, m, 8));
    assert(r.requests == 2 && r.last_page == 81);
    assert(slot(&r, NULL) == 81);            /* immediately, not after the 80 */
    sent = 1;
    for (int i = 0; i < 64; i++) {
        uint8_t p = slot(&r, NULL);
        assert(p == 0 || p == 81);           /* the old page is gone */
        if (p == 81) sent++;
    }
    assert(sent == 3);

    printf("test_ant_request: all tests passed\n");
    return 0;
}
