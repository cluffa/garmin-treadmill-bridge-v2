#pragma once
#include <stdbool.h>
#include <stdint.h>

/* ANT+ common page 70 ("Request Data Page"), sensor side.
 *
 * Why this exists — docs/sdm-recording-analysis.md §12. The footpod broadcasts
 * capabilities page 22 declaring cadence unsupported, and a recorded run proved
 * the watch ignores it: it kept taking cadence from us and recording a flat
 * zero, poisoning power, step length and running dynamics. The surviving
 * explanation is that a display learns capabilities by *asking* — page 70 — and
 * discards unsolicited arrivals. The SDK's own SDM master emits page 22 only
 * from its request controller and never from the periodic rotation, which is
 * the same asymmetry seen from the other side.
 *
 * So this is the sensor half of that exchange: decode a received page 70 and
 * decide which page to put on which TX slot in reply. Pure and host-testable;
 * the ANT plumbing is firmware/ant_sdm.c.
 *
 * The diagnostic counters are not decoration. `requests` answers the question
 * a recorded run cannot: whether the watch ever asks at all. It is reported by
 * the console's STATUS as "p70", so a re-pair with the console attached
 * discriminates "the watch asks and we never answered" from "the watch never
 * asks, and no capabilities work can fix cadence" — without a treadmill, a
 * workout, or a .FIT download. */

#define ANT_COMMON_PAGE_70   70

/* TX slots between consecutive responses. The footpod broadcasts at ~4 Hz and
 * the watch records one sample per second, so four slots is one record.
 *
 * A response displaces the rotation page that slot would have carried, which
 * matters: firmware/ant_sdm.c interleaves its background pages so that at
 * least three of any four consecutive slots are page 1 (speed), because a
 * one-second window with no page 1 shows up in the recording as a speed hole.
 * Spreading responses one per four slots bounds the damage — a window can hold
 * one response plus the rotation's own background page, leaving two page 1s.
 * Answering a burst back-to-back would leave none. */
#define ANT_REQUEST_SPREAD   4

/* Caps on how long one request can keep the interleave degraded. The SDK has
 * no equivalent because it has no interleave invariant to protect; these are a
 * deliberate divergence. At ANT_REQUEST_SPREAD slots apiece these are ~16 s
 * and ~8 s of bounded degradation respectively. */
#define ANT_REQUEST_MAX_COUNT      16   /* explicit transmission count */
#define ANT_REQUEST_MAX_UNTIL_ACK   8   /* 0x80 "transmit until acknowledged" */

typedef struct {
    uint8_t  page;        /* page being answered */
    uint8_t  remaining;   /* responses still owed; 0 = idle */
    bool     ack;         /* answer with acknowledged, not broadcast, messages */
    bool     until_ack;   /* 0x80: one successful transfer discharges the rest */
    uint8_t  hold;        /* TX slots to let pass before the next response */

    /* ---- diagnostics, surfaced by STATUS ---- */
    uint32_t requests;    /* page 70s accepted and acted on */
    uint32_t rejects;     /* page 70s understood but not actionable */
    uint8_t  last_page;   /* page number the last accepted request asked for */
} ant_request_t;

void ant_request_init(ant_request_t *r);

/* Can the footpod build this page at all? Only the five it broadcasts. The
 * profile's instruction for anything else is to ignore the request rather than
 * answer it with something wrong. */
bool ant_request_page_supported(uint8_t page);

/* Feed a message received on the footpod's channel. Returns true if it was an
 * actionable page 70, in which case responses are now owed. A message that is
 * not a page 70 at all is not counted as a reject — the watch's own traffic
 * lands here too, and `requests`/`rejects` have to keep meaning what STATUS
 * says they mean. */
bool ant_request_on_rx(ant_request_t *r, const uint8_t *msg, uint8_t len);

/* Called once per TX slot. Returns true if this slot should carry a requested
 * page instead of the rotation's, writing the page number and whether it must
 * go out acknowledged. */
bool ant_request_take_slot(ant_request_t *r, uint8_t *page, bool *ack);

/* Outcome of an acknowledged response: EVENT_TRANSFER_TX_COMPLETED (ok) or
 * EVENT_TRANSFER_TX_FAILED (!ok). A failure just leaves the remaining
 * transmissions to run; only the until-acknowledged mode ends early on
 * success, matching the SDK's request controller. */
void ant_request_on_response_ack(ant_request_t *r, bool ok);

/* Hand a slot back when the radio refused the message outright, so the
 * transmission still counts as owed. Distinct from a failed *transfer*
 * (on_response_ack above): nothing reached the air here, so retrying it is not
 * an extra transmission, and dropping it silently would under-answer a request
 * whose whole point is that the display is waiting. */
void ant_request_retry_slot(ant_request_t *r);
