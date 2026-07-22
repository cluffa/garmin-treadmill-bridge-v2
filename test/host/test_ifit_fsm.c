#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "ifit_fsm.h"

/* Capture every frame the FSM emits in one tick. */
#define MAX_FRAMES 8
static uint8_t frames[MAX_FRAMES][32];
static size_t  frame_len[MAX_FRAMES];
static int     nframes;

static void capture(const uint8_t *f, size_t len, void *ctx)
{
    (void)ctx;
    assert(nframes < MAX_FRAMES);
    assert(len <= sizeof frames[0]);
    memcpy(frames[nframes], f, len);
    frame_len[nframes] = len;
    nframes++;
}

static void tick(void) { nframes = 0; ifit_fsm_tick(capture, NULL); }

int main(void)
{
    ifit_fsm_reset();

    /* 18 init ticks: exactly one frame each; first is the fe/02/08/02 noop,
     * second starts with 0xff. */
    tick();
    assert(nframes == 1 && frame_len[0] == 4);
    const uint8_t init0[4] = {0xfe, 0x02, 0x08, 0x02};
    assert(memcmp(frames[0], init0, 4) == 0);
    for (int i = 1; i < 18; i++) {
        tick();
        assert(nframes == 1);
    }

    /* Poll cycle begins: phases 0..5, one frame per tick when idle. */
    tick();                                   /* phase 0 */
    assert(nframes == 1 && frames[0][0] == 0xfe);
    tick();                                   /* phase 1 */
    assert(nframes == 1 && frames[0][0] == 0x00);

    /* Request 8.0 km/h while belt is stopped: phase 2 must NOT emit a
     * control frame (belt not moving) — instead START_SEQ fires at phase 5. */
    ifit_fsm_request_speed(8.0f);
    tick();                                   /* phase 2: poll frame only */
    assert(nframes == 1 && frames[0][0] == 0xff);
    tick();                                   /* phase 3 */
    assert(nframes == 1);
    tick();                                   /* phase 4 */
    assert(nframes == 1);
    tick();                                   /* phase 5: poll + 5 START frames */
    assert(nframes == 6);
    assert(frames[1][0] == 0xfe && frames[1][2] == 0x20);  /* START_01 */
    assert(frames[5][0] == 0xff && frames[5][1] == 0x11);  /* START_05 */

    /* Belt starts moving; pending 8.0 km/h applies at the next phase 2:
     * poll frame + noop + forceSpeed. */
    ifit_fsm_note_speed(1.0f);
    tick();                                   /* phase 0 */
    tick();                                   /* phase 1 */
    tick();                                   /* phase 2 */
    assert(nframes == 3);
    const uint8_t noop[4] = {0xfe, 0x02, 0x0d, 0x02};
    assert(frame_len[1] == 4 && memcmp(frames[1], noop, 4) == 0);
    /* forceSpeed: kind=0x01 @b10, 800 (8.00 km/h) LE @b11-12, cksum=lo+0x12 */
    assert(frame_len[2] == 20);
    assert(frames[2][10] == 0x01);
    assert(frames[2][11] == 0x20 && frames[2][12] == 0x03);
    assert(frames[2][14] == (uint8_t)(0x20 + 0x12));

    /* Request consumed: next phase 2 is clean. */
    for (int i = 0; i < 6; i++) tick();       /* phases 3,4,5,0,1,2 */
    assert(nframes == 1);

    /* Incline 5 % injects kind=0x02, 500 LE, cksum lo+0x12. */
    ifit_fsm_request_incline(5.0f);
    for (int i = 0; i < 5; i++) tick();       /* 3,4,5,0,1 */
    tick();                                   /* phase 2 */
    assert(nframes == 3);
    assert(frames[2][10] == 0x02);
    assert(frames[2][11] == (uint8_t)(500 & 0xFF));
    assert(frames[2][12] == (uint8_t)(500 >> 8));
    assert(frames[2][14] == (uint8_t)((500 & 0xFF) + 0x12));

    /* Stop = speed 0: applies immediately at phase 2 even while moving. */
    ifit_fsm_request_stop();
    for (int i = 0; i < 6; i++) tick();       /* 3,4,5,0,1,2 */
    assert(nframes == 3);
    assert(frames[2][10] == 0x01 && frames[2][11] == 0 && frames[2][12] == 0);

    /* Out-of-range guards: >22 km/h and negative incline are not sent. */
    ifit_fsm_request_speed(25.0f);
    ifit_fsm_note_speed(8.0f);
    for (int i = 0; i < 6; i++) tick();
    assert(nframes == 1);                     /* rejected, nothing injected */

    /* reset() restarts the init sequence. */
    ifit_fsm_reset();
    tick();
    assert(nframes == 1 && memcmp(frames[0], init0, 4) == 0);

    printf("ifit_fsm: OK\n");
    return 0;
}
