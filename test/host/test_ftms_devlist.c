#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "ftms_devlist.h"

static ftms_device_t mk(uint8_t a0, int8_t rssi, const char *name) {
    ftms_device_t d = {0};
    d.addr[0] = a0; d.addr_type = 1; d.rssi = rssi;
    snprintf(d.name, FTMS_NAME_LEN, "%s", name);
    return d;
}

int main(void) {
    ftms_device_t list[FTMS_MAX_DEVICES];
    int n = 0;
    ftms_device_t a = mk(0x11, -80, "A");
    ftms_device_t b = mk(0x22, -50, "B");
    n = ftms_devlist_upsert(list, n, &a); assert(n == 1);
    n = ftms_devlist_upsert(list, n, &b); assert(n == 2);
    // upsert same addr as a updates rssi/name in place, no new entry
    ftms_device_t a2 = mk(0x11, -60, "A2");
    n = ftms_devlist_upsert(list, n, &a2); assert(n == 2);
    // find a's slot and confirm it updated
    for (int i = 0; i < n; i++)
        if (list[i].addr[0] == 0x11) { assert(list[i].rssi == -60); assert(strcmp(list[i].name,"A2")==0); }
    // a nameless report (UUID-only advert) updates rssi but must NOT blank the name
    ftms_device_t a3 = mk(0x11, -55, "");
    n = ftms_devlist_upsert(list, n, &a3); assert(n == 2);
    for (int i = 0; i < n; i++)
        if (list[i].addr[0] == 0x11) { assert(list[i].rssi == -55); assert(strcmp(list[i].name,"A2")==0); }
    // sort by rssi desc: B(-50) before A(-60)
    ftms_devlist_sort(list, n);
    assert(list[0].addr[0] == 0x22 && list[1].addr[0] == 0x11);
    // full table: appends stop at FTMS_MAX_DEVICES
    n = 0;
    for (int i = 0; i < FTMS_MAX_DEVICES + 3; i++) {
        ftms_device_t d = mk((uint8_t)(0x30+i), (int8_t)(-i), "x");
        n = ftms_devlist_upsert(list, n, &d);
    }
    assert(n == FTMS_MAX_DEVICES);
    printf("ftms_devlist: OK\n");
    return 0;
}
