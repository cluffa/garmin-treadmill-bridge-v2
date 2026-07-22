#include "connect_policy.h"
#include <string.h>

static int find_addr(const ftms_device_t *list, int n, const uint8_t addr[6])
{
    for (int i = 0; i < n; i++)
        if (memcmp(list[i].addr, addr, 6) == 0) return i;
    return -1;
}

static int best_rssi(const ftms_device_t *list, int n)
{
    int best = 0;
    for (int i = 1; i < n; i++)
        if (list[i].rssi > list[best].rssi) best = i;
    return best;
}

int connect_policy_choose(const ftms_device_t *list, int n,
                          const ftms_device_t *saved,
                          uint32_t ms_since_scan_start)
{
    if (n <= 0) return -1;

    if (saved != NULL) {
        int i = find_addr(list, n, saved->addr);
        if (i >= 0) return i;
        if (ms_since_scan_start < CONNECT_POLICY_SAVED_WAIT_MS) return -1;
        return best_rssi(list, n);
    }

    if (ms_since_scan_start < CONNECT_POLICY_PICK_WINDOW_MS) return -1;
    return best_rssi(list, n);
}
