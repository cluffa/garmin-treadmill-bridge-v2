/*
 * last_device.c — FDS-backed persistence of the last-connected treadmill.
 *
 * Adapted from the ESP32 build's NVS "ftms/last" blob concept. The nRF
 * equivalent uses Flash Data Storage (fds). Saves are skipped when the
 * stored record already matches the current device, so steady-state
 * reconnects don't wear flash.
 */

#include "last_device.h"

#include "fds.h"
#include "nrf_log.h"
#include "nrf_pwr_mgmt.h"

#include <string.h>

#define LAST_FILE_ID  0x7452   /* "tR" */
#define LAST_REC_KEY  0x0001

/* fds needs word-aligned data that stays valid until FDS_EVT_WRITE. */
typedef union {
    ftms_device_t dev;
    uint32_t      words[(sizeof(ftms_device_t) + 3) / 4];
} record_buf_t;

static record_buf_t   s_buf;
static volatile bool  s_fds_ready;
static volatile bool  s_write_pending;

static void fds_evt_handler(const fds_evt_t *evt)
{
    switch (evt->id) {
    case FDS_EVT_INIT:
        s_fds_ready = (evt->result == NRF_SUCCESS);
        break;
    case FDS_EVT_WRITE:
    case FDS_EVT_UPDATE:
        s_write_pending = false;
        if (evt->result != NRF_SUCCESS) {
            NRF_LOG_WARNING("last_device: save failed (0x%x)",
                            (unsigned int)evt->result);
        }
        break;
    case FDS_EVT_GC:
        NRF_LOG_INFO("last_device: flash GC complete");
        break;
    default:
        break;
    }
}

void last_device_init(void)
{
    APP_ERROR_CHECK(fds_register(fds_evt_handler));
    APP_ERROR_CHECK(fds_init());
    while (!s_fds_ready) {
        nrf_pwr_mgmt_run();
    }
    NRF_LOG_INFO("last_device: fds ready");
}

bool last_device_load(ftms_device_t *out)
{
    fds_record_desc_t desc;
    fds_find_token_t  tok;
    memset(&tok, 0, sizeof tok);

    if (fds_record_find(LAST_FILE_ID, LAST_REC_KEY, &desc, &tok) != NRF_SUCCESS)
        return false;

    fds_flash_record_t rec;
    if (fds_record_open(&desc, &rec) != NRF_SUCCESS) return false;

    bool ok = rec.p_header->length_words * 4 >= sizeof(ftms_device_t);
    if (ok) memcpy(out, rec.p_data, sizeof(ftms_device_t));
    (void)fds_record_close(&desc);
    return ok;
}

void last_device_save(const ftms_device_t *d)
{
    if (s_write_pending) return;   /* previous save still in flight */

    ftms_device_t cur;
    bool have = last_device_load(&cur);
    if (have && memcmp(cur.addr, d->addr, 6) == 0 &&
        cur.addr_type == d->addr_type && cur.proto == d->proto) {
        return;   /* unchanged — spare the flash */
    }

    memset(&s_buf, 0, sizeof s_buf);
    s_buf.dev = *d;

    fds_record_t rec = {
        .file_id = LAST_FILE_ID,
        .key     = LAST_REC_KEY,
        .data    = {
            .p_data       = s_buf.words,
            .length_words = sizeof s_buf.words / sizeof s_buf.words[0],
        },
    };

    ret_code_t err;
    if (have) {
        fds_record_desc_t desc;
        fds_find_token_t  tok;
        memset(&tok, 0, sizeof tok);
        (void)fds_record_find(LAST_FILE_ID, LAST_REC_KEY, &desc, &tok);
        err = fds_record_update(&desc, &rec);
    } else {
        err = fds_record_write(NULL, &rec);
    }

    if (err == FDS_ERR_NO_SPACE_IN_FLASH) {
        (void)fds_gc();   /* reclaim; the next connect will save again */
        NRF_LOG_WARNING("last_device: flash full — GC queued");
        return;
    }
    if (err == NRF_SUCCESS) {
        s_write_pending = true;
        NRF_LOG_INFO("last_device: saving \"%s\"", nrf_log_push((char *)d->name));
    } else {
        NRF_LOG_WARNING("last_device: save err 0x%x", (unsigned int)err);
    }
}
