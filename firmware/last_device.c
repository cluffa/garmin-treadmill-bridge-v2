/*
 * last_device.c — FDS-backed persistence of the last-connected treadmill.
 *
 * Adapted from the ESP32 build's NVS "ftms/last" blob concept. The nRF
 * equivalent uses Flash Data Storage (fds). Saves are skipped when the
 * stored record already matches the current device, so steady-state
 * reconnects don't wear flash.
 */

#include "last_device.h"

#include "app_timer.h"
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
static volatile bool  s_fds_init_signaled;  /* FDS_EVT_INIT received at all? */
static volatile bool  s_fds_unavailable;    /* FDS permanently down */
static volatile bool  s_write_pending;

static void fds_evt_handler(const fds_evt_t *evt)
{
    switch (evt->id) {
    case FDS_EVT_INIT:
        s_fds_init_signaled = true;
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

    /* Wait up to 3 seconds for FDS to initialise.  app_timer is driven by
     * RTC1 at 32768 Hz and is already running by the time we're called
     * (app_timer_init() runs in main before ble_central_init).
     * app_timer_cnt_diff_compute() handles the 24-bit counter wrap.
     * 3 s is far longer than a normal FDS init (~tens of ms) but short
     * enough that the user won't assume the board is dead. */
#define LAST_DEVICE_FDS_TIMEOUT_TICKS APP_TIMER_TICKS(3000)
    uint32_t start = app_timer_cnt_get();
    while (!s_fds_ready) {
        if (app_timer_cnt_diff_compute(app_timer_cnt_get(), start) >=
            LAST_DEVICE_FDS_TIMEOUT_TICKS) {
            break;
        }
        nrf_pwr_mgmt_run();
    }
#undef LAST_DEVICE_FDS_TIMEOUT_TICKS

    if (s_fds_ready) {
        NRF_LOG_INFO("last_device: fds ready");
    } else {
        s_fds_unavailable = true;
        if (s_fds_init_signaled) {
            NRF_LOG_ERROR(
                "last_device: FDS init reported error — persistence disabled");
        } else {
            NRF_LOG_ERROR(
                "last_device: FDS init timed out — persistence disabled");
        }
    }
}

bool last_device_load(ftms_device_t *out)
{
    if (s_fds_unavailable) return false;

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
    if (s_fds_unavailable) return;
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
    fds_record_desc_t desc;
    fds_find_token_t  tok;
    memset(&tok, 0, sizeof tok);

    /* Only update in place if the record is actually found — otherwise `desc`
     * is uninitialized and fds_record_update() would corrupt flush state.
     * `have` reflects a prior load, but the descriptor must come from a fresh
     * find, so re-find and fall back to write if it's gone. */
    if (fds_record_find(LAST_FILE_ID, LAST_REC_KEY, &desc, &tok) == NRF_SUCCESS) {
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
