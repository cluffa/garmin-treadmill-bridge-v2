/* Host tests for connect_policy_choose() — the auto-connect chooser.
 *
 * Policy under test (see connect_policy.h):
 *  - The saved (last-connected) device wins the moment it is seen.
 *  - With a saved device unseen, hold out CONNECT_POLICY_SAVED_WAIT_MS
 *    before settling for the best available.
 *  - With no saved device, collect for CONNECT_POLICY_PICK_WINDOW_MS then
 *    pick the strongest RSSI (ties: earliest entry, i.e. first found).
 */
#include "connect_policy.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static ftms_device_t dev(uint8_t last_octet, int8_t rssi)
{
    ftms_device_t d;
    memset(&d, 0, sizeof d);
    d.addr[5] = last_octet;
    d.rssi = rssi;
    d.proto = MACHINE_PROTO_FTMS;
    return d;
}

static void test_empty_list_never_picks(void)
{
    assert(connect_policy_choose(NULL, 0, NULL, 0) == -1);
    assert(connect_policy_choose(NULL, 0, NULL, 60000) == -1);
}

static void test_saved_device_picked_immediately(void)
{
    ftms_device_t list[3] = { dev(1, -80), dev(2, -40), dev(3, -60) };
    ftms_device_t saved = dev(3, -127);   /* rssi in saved copy is stale */
    /* Even at t=0 and even though idx 1 is closer, the saved device wins. */
    assert(connect_policy_choose(list, 3, &saved, 0) == 2);
}

static void test_saved_unseen_holds_out_then_falls_back(void)
{
    ftms_device_t list[2] = { dev(1, -70), dev(2, -50) };
    ftms_device_t saved = dev(9, -50);    /* not in the list */
    assert(connect_policy_choose(list, 2, &saved, 0) == -1);
    assert(connect_policy_choose(list, 2, &saved,
                                 CONNECT_POLICY_SAVED_WAIT_MS - 1) == -1);
    /* After the wait expires: strongest RSSI (idx 1). */
    assert(connect_policy_choose(list, 2, &saved,
                                 CONNECT_POLICY_SAVED_WAIT_MS) == 1);
}

static void test_no_saved_waits_pick_window_then_best_rssi(void)
{
    ftms_device_t list[3] = { dev(1, -70), dev(2, -50), dev(3, -90) };
    assert(connect_policy_choose(list, 3, NULL, 0) == -1);
    assert(connect_policy_choose(list, 3, NULL,
                                 CONNECT_POLICY_PICK_WINDOW_MS - 1) == -1);
    assert(connect_policy_choose(list, 3, NULL,
                                 CONNECT_POLICY_PICK_WINDOW_MS) == 1);
}

static void test_rssi_tie_prefers_first_found(void)
{
    ftms_device_t list[3] = { dev(1, -60), dev(2, -60), dev(3, -60) };
    assert(connect_policy_choose(list, 3, NULL,
                                 CONNECT_POLICY_PICK_WINDOW_MS) == 0);
}

static void test_single_device_still_waits_for_saved(void)
{
    /* One (wrong) device visible must not preempt the saved-device wait. */
    ftms_device_t list[1] = { dev(1, -40) };
    ftms_device_t saved = dev(9, -50);
    assert(connect_policy_choose(list, 1, &saved, 1000) == -1);
    assert(connect_policy_choose(list, 1, &saved,
                                 CONNECT_POLICY_SAVED_WAIT_MS) == 0);
}

int main(void)
{
    test_empty_list_never_picks();
    test_saved_device_picked_immediately();
    test_saved_unseen_holds_out_then_falls_back();
    test_no_saved_waits_pick_window_then_best_rssi();
    test_rssi_tie_prefers_first_found();
    test_single_device_still_waits_for_saved();
    printf("test_connect_policy: all tests passed\n");
    return 0;
}
