#pragma once
#include <stdint.h>

#define FTMS_MAX_DEVICES 8
#define FTMS_NAME_LEN    20

/* Which machine-side protocol a discovered device speaks. */
enum { MACHINE_PROTO_FTMS = 0, MACHINE_PROTO_IFIT = 1 };

typedef struct {
    uint8_t addr[6];
    uint8_t addr_type;
    int8_t  rssi;
    uint8_t proto;                /* MACHINE_PROTO_* */
    char    name[FTMS_NAME_LEN];
} ftms_device_t;

/* Update the entry matching d->addr in place (rssi+name), else append if there
 * is room. Returns the new count. */
int  ftms_devlist_upsert(ftms_device_t *list, int n, const ftms_device_t *d);

/* Sort the first n entries by rssi, strongest (largest) first. */
void ftms_devlist_sort(ftms_device_t *list, int n);
