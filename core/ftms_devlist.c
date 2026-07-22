#include "ftms_devlist.h"
#include <string.h>

static int find(const ftms_device_t *list, int n, const uint8_t addr[6]) {
    for (int i = 0; i < n; i++)
        if (memcmp(list[i].addr, addr, 6) == 0) return i;
    return -1;
}

int ftms_devlist_upsert(ftms_device_t *list, int n, const ftms_device_t *d) {
    int i = find(list, n, d->addr);
    if (i >= 0) {
        list[i].rssi = d->rssi;
        /* Keep an already-captured name: the UUID and the name often arrive in
         * different reports (primary advert vs scan response), so don't let a
         * nameless report blank out a name we already have. */
        if (d->name[0]) memcpy(list[i].name, d->name, FTMS_NAME_LEN);
        return n;
    }
    if (n >= FTMS_MAX_DEVICES) return n;
    list[n] = *d;
    return n + 1;
}

void ftms_devlist_sort(ftms_device_t *list, int n) {
    for (int i = 1; i < n; i++) {           /* insertion sort, rssi desc */
        ftms_device_t key = list[i];
        int j = i - 1;
        while (j >= 0 && list[j].rssi < key.rssi) { list[j+1] = list[j]; j--; }
        list[j+1] = key;
    }
}
