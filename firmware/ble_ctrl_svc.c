/*
 * ble_ctrl_svc.c — S340 BLE peripheral GATT server: watch control side.
 *
 * Vendor service A6ED0001-2E7A-4E1D-9E3B-000000000000 with 3 characteristics:
 *   A6ED0002 (write)     → route bytes to ctrl_dispatch()
 *   A6ED0003 (notify)    → compact D/E/S frames (≤ 20 B, CIQ MTU is 23)
 *   A6ED0004 (write)     → raw 15-byte workout frame → workout_ctrl_on_frame()
 *
 * Advertising carries the A6ED service UUID in the primary ADV packet;
 * the device name goes in the scan response.
 */

#include "ble_ctrl_svc.h"

#include "app_error.h"
#include "app_state.h"
#include "ble_advdata.h"
#include "ble_gap.h"
#include "ble_gatts.h"
#include "ble_srv_common.h"
#include "ctrl_dispatch.h"
#include "ctrl_frames.h"
#include "machine.h"
#include "nrf_log.h"
#include "nrf_sdh_ble.h"
#include "workout_ctrl.h"

#include <string.h>

#define DEVICE_NAME     "TMILL-CTRL"
#define CONN_CFG_TAG    1

/* A6ED0000-2E7A-4E1D-9E3B-000000000000  — little-endian BLE byte order,
 * bytes 12-13 hold the 16-bit alias placeholder (0000). */
static const ble_uuid128_t CTRL_BASE = {{
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x3B, 0x9E, 0x1D, 0x4E, 0x7A, 0x2E,
    0x00, 0x00, 0xED, 0xA6
}};

#define CTRL_SVC_UUID   0x0001   /* → A6ED0001-… */
#define CTRL_CHR_UUID   0x0002   /* → A6ED0002-…  write (ctrl grammar) */
#define CTRL_RSP_UUID   0x0003   /* → A6ED0003-…  notify (D/E/S frames) */
#define CTRL_WKT_UUID   0x0004   /* → A6ED0004-…  write (workout telemetry) */

#define CTRL_CHR_MAX_LEN  64     /* longest ctrl_dispatch line accepted */
#define CTRL_WKT_MAX_LEN  32     /* workout frame + headroom */

/* Low-duty connectable advertising: ~285 ms interval (456 * 0.625 ms).
 * Leaves air time for the ANT and treadmill central links. */
#define ADV_INTERVAL      456

static uint8_t  s_uuid_type;
static uint16_t s_svc_handle;
static ble_gatts_char_handles_t s_chr_handles;
static ble_gatts_char_handles_t s_rsp_handles;
static ble_gatts_char_handles_t s_wkt_handles;
static uint16_t s_conn_handle = BLE_CONN_HANDLE_INVALID;
static bool     s_notify_on;

static uint8_t s_adv_handle = BLE_GAP_ADV_SET_HANDLE_NOT_SET;
static uint8_t s_adv_buf[BLE_GAP_ADV_SET_DATA_SIZE_MAX];
static uint8_t s_srsp_buf[BLE_GAP_ADV_SET_DATA_SIZE_MAX];
static ble_gap_adv_data_t s_adv_data = {
    .adv_data      = { .p_data = s_adv_buf,  .len = sizeof(s_adv_buf)  },
    .scan_rsp_data = { .p_data = s_srsp_buf, .len = sizeof(s_srsp_buf) },
};

/* ---- Notification TX queue ---------------------------------------------------
 * The SoftDevice buffers only a few notifications per interval; a LIST reply
 * is up to FTMS_MAX_DEVICES+1 frames, so ring-buffer them and drain on
 * BLE_GATTS_EVT_HVN_TX_COMPLETE. */
#define TXQ_LEN  12
static struct { uint8_t len; uint8_t data[CTRL_FRAME_MAX]; } s_txq[TXQ_LEN];
static volatile uint8_t s_txq_head, s_txq_tail;

static void txq_pump(void)
{
    while (s_txq_tail != s_txq_head) {
        if (s_conn_handle == BLE_CONN_HANDLE_INVALID || !s_notify_on) {
            s_txq_tail = s_txq_head;   /* drop — nobody listening */
            return;
        }
        uint16_t len = s_txq[s_txq_tail].len;
        ble_gatts_hvx_params_t hvx = {
            .handle = s_rsp_handles.value_handle,
            .type   = BLE_GATT_HVX_NOTIFICATION,
            .offset = 0,
            .p_len  = &len,
            .p_data = s_txq[s_txq_tail].data,
        };
        uint32_t err = sd_ble_gatts_hvx(s_conn_handle, &hvx);
        if (err == NRF_ERROR_RESOURCES) return;   /* resume on TX_COMPLETE */
        if (err != NRF_SUCCESS) {
            NRF_LOG_WARNING("ctrl_svc: hvx err 0x%x — dropping frame",
                            (unsigned int)err);
        }
        s_txq_tail = (uint8_t)((s_txq_tail + 1) % TXQ_LEN);
    }
}

static void txq_push(const uint8_t *data, uint8_t len)
{
    if (len > CTRL_FRAME_MAX) return;

    uint8_t next = (uint8_t)((s_txq_head + 1) % TXQ_LEN);
    if (next == s_txq_tail) {
        NRF_LOG_WARNING("ctrl_svc: tx queue full — frame dropped");
        return;
    }
    s_txq[s_txq_head].len = len;
    memcpy(s_txq[s_txq_head].data, data, len);
    s_txq_head = next;
    txq_pump();
}

/* ---- Watch-facing compact frames -------------------------------------------- */

static void send_status_frame(void)
{
    const ftms_device_t *dev = machine_connected_device();
    uint8_t buf[CTRL_FRAME_MAX];
    int n = ctrl_frame_status(buf, dev != NULL,
                              dev ? dev->proto : 0,
                              dev ? dev->name : NULL);
    txq_push(buf, (uint8_t)n);
}

static void send_list_frames(void)
{
    ftms_device_t devs[FTMS_MAX_DEVICES];
    int n = machine_get_devices(devs, FTMS_MAX_DEVICES);

    const ftms_device_t *live = machine_connected_device();
    ftms_device_t saved;
    bool have_saved = machine_saved_device(&saved);

    for (int i = 0; i < n; i++) {
        uint8_t flags = 0;
        if (live && memcmp(devs[i].addr, live->addr, 6) == 0)
            flags |= CTRL_DEV_FLAG_CONNECTED;
        if (have_saved && memcmp(devs[i].addr, saved.addr, 6) == 0)
            flags |= CTRL_DEV_FLAG_SAVED;
        uint8_t buf[CTRL_FRAME_MAX];
        int len = ctrl_frame_device(buf, (uint8_t)i, &devs[i], flags);
        txq_push(buf, (uint8_t)len);
    }
    uint8_t buf[CTRL_FRAME_MAX];
    int len = ctrl_frame_list_end(buf, (uint8_t)n);
    txq_push(buf, (uint8_t)len);
}

/* ctrl_dispatch responses go to log — the watch only gets compact 'D'/'E'/'S'
 * frames (a full JSON line exceeds CIQ's 20-byte notification payload). */
static void ctrl_log_tx(const char *msg, void *ctx)
{
    (void)ctx;
    NRF_LOG_INFO("ctrl: %s", nrf_log_push((char *)msg));
}

/* ---- BLE event handling ----------------------------------------------------- */

static void on_write(const ble_gatts_evt_write_t *w)
{
    if (w->handle == s_rsp_handles.cccd_handle && w->len >= 2) {
        s_notify_on = (w->data[0] & 0x01) != 0;
        NRF_LOG_INFO("ctrl_svc: notifications %s",
                     s_notify_on ? "on" : "off");
        if (s_notify_on) send_status_frame();   /* greet with link state */
        return;
    }
    /* Workout telemetry char: raw binary frame → shared control policy. */
    if (w->handle == s_wkt_handles.value_handle) {
        workout_ctrl_on_frame(w->data, w->len);
        return;
    }
    if (w->handle != s_chr_handles.value_handle) return;

    char line[CTRL_CHR_MAX_LEN + 1];
    uint16_t n = w->len < CTRL_CHR_MAX_LEN ? w->len : CTRL_CHR_MAX_LEN;
    memcpy(line, w->data, n);
    line[n] = '\0';
    NRF_LOG_INFO("ctrl_svc: rx \"%s\"", nrf_log_push(line));

    /* LIST and STATUS answer the watch in compact frames; everything else
     * (SCAN/CONNECT/SPEED/STOP) runs through the shared grammar. */
    if (strcmp(line, "LIST") == 0) {
        send_list_frames();
        return;
    }
    if (strcmp(line, "STATUS") == 0) {
        send_status_frame();
        return;
    }
    ctrl_dispatch(line, ctrl_log_tx, NULL);
}

static void ble_evt_handler(const ble_evt_t *p_evt, void *p_ctx)
{
    (void)p_ctx;
    const ble_gap_evt_t *gap = &p_evt->evt.gap_evt;

    switch (p_evt->header.evt_id) {
    case BLE_GAP_EVT_CONNECTED:
        if (gap->params.connected.role != BLE_GAP_ROLE_PERIPH) break;
        s_conn_handle = gap->conn_handle;
        s_notify_on = false;
        s_txq_head = s_txq_tail = 0;
        app_state()->watch_connected = true;
        NRF_LOG_INFO("ctrl_svc: watch connected (handle %u)", s_conn_handle);
        break;

    case BLE_GAP_EVT_DISCONNECTED:
        if (gap->conn_handle != s_conn_handle) break;
        s_conn_handle = BLE_CONN_HANDLE_INVALID;
        s_notify_on = false;
        app_state()->watch_connected = false;
        workout_ctrl_reset();   /* stop re-asserting a stale target */
        NRF_LOG_INFO("ctrl_svc: watch disconnected — re-advertising");
        ble_ctrl_svc_advertise_start();
        break;

    case BLE_GATTS_EVT_WRITE:
        if (p_evt->evt.gatts_evt.conn_handle == s_conn_handle) {
            on_write(&p_evt->evt.gatts_evt.params.write);
        }
        break;

    case BLE_GATTS_EVT_HVN_TX_COMPLETE:
        if (p_evt->evt.gatts_evt.conn_handle == s_conn_handle) {
            txq_pump();
        }
        break;

    default:
        break;
    }
}

NRF_SDH_BLE_OBSERVER(m_ctrl_svc_obs, 3 /* prio */, ble_evt_handler, NULL);

/* ---- Service registration --------------------------------------------------- */

static void service_init(void)
{
    APP_ERROR_CHECK(sd_ble_uuid_vs_add(&CTRL_BASE, &s_uuid_type));

    ble_uuid_t svc_uuid = { .uuid = CTRL_SVC_UUID, .type = s_uuid_type };
    APP_ERROR_CHECK(sd_ble_gatts_service_add(BLE_GATTS_SRVC_TYPE_PRIMARY,
                                              &svc_uuid, &s_svc_handle));

    /* Control characteristic: write / write-no-response. */
    ble_gatts_char_md_t char_md = {
        .char_props = { .write = 1, .write_wo_resp = 1 },
    };
    ble_gatts_attr_md_t attr_md = {
        .vloc = BLE_GATTS_VLOC_STACK,
        .vlen = 1,
    };
    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&attr_md.read_perm);
    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&attr_md.write_perm);

    ble_uuid_t chr_uuid = { .uuid = CTRL_CHR_UUID, .type = s_uuid_type };
    ble_gatts_attr_t attr = {
        .p_uuid    = &chr_uuid,
        .p_attr_md = &attr_md,
        .init_len  = 0,
        .max_len   = CTRL_CHR_MAX_LEN,
    };
    APP_ERROR_CHECK(sd_ble_gatts_characteristic_add(s_svc_handle, &char_md,
                                                     &attr, &s_chr_handles));

    /* Response characteristic: notify-only, CCCD open for the watch. */
    ble_gatts_attr_md_t cccd_md = {
        .vloc = BLE_GATTS_VLOC_STACK,
    };
    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&cccd_md.read_perm);
    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&cccd_md.write_perm);

    ble_gatts_char_md_t rsp_md = {
        .char_props = { .notify = 1 },
        .p_cccd_md  = &cccd_md,
    };
    ble_gatts_attr_md_t rsp_attr_md = {
        .vloc = BLE_GATTS_VLOC_STACK,
        .vlen = 1,
    };
    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&rsp_attr_md.read_perm);
    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&rsp_attr_md.write_perm);

    ble_uuid_t rsp_uuid = { .uuid = CTRL_RSP_UUID, .type = s_uuid_type };
    ble_gatts_attr_t rsp_attr = {
        .p_uuid    = &rsp_uuid,
        .p_attr_md = &rsp_attr_md,
        .init_len  = 0,
        .max_len   = CTRL_FRAME_MAX,
    };
    APP_ERROR_CHECK(sd_ble_gatts_characteristic_add(s_svc_handle, &rsp_md,
                                                     &rsp_attr, &s_rsp_handles));

    /* Workout telemetry characteristic: the data field writes raw binary
     * frames (write / write-no-response), decoded by workout_ctrl. */
    ble_gatts_char_md_t wkt_md = {
        .char_props = { .write = 1, .write_wo_resp = 1 },
    };
    ble_gatts_attr_md_t wkt_attr_md = {
        .vloc = BLE_GATTS_VLOC_STACK,
        .vlen = 1,
    };
    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&wkt_attr_md.read_perm);
    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&wkt_attr_md.write_perm);

    ble_uuid_t wkt_uuid = { .uuid = CTRL_WKT_UUID, .type = s_uuid_type };
    ble_gatts_attr_t wkt_attr = {
        .p_uuid    = &wkt_uuid,
        .p_attr_md = &wkt_attr_md,
        .init_len  = 0,
        .max_len   = CTRL_WKT_MAX_LEN,
    };
    APP_ERROR_CHECK(sd_ble_gatts_characteristic_add(s_svc_handle, &wkt_md,
                                                     &wkt_attr, &s_wkt_handles));
}

/* ---- Advertising ------------------------------------------------------------ */

static void advertising_init(void)
{
    ble_gap_conn_sec_mode_t sec;
    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&sec);
    APP_ERROR_CHECK(sd_ble_gap_device_name_set(&sec,
                                               (const uint8_t *)DEVICE_NAME,
                                               strlen(DEVICE_NAME)));

    /* Advert: flags + the 128-bit service UUID (fills most of the 31 B);
     * the name goes in the scan response. */
    ble_uuid_t adv_uuid = { .uuid = CTRL_SVC_UUID, .type = s_uuid_type };
    ble_advdata_t advdata = {
        .flags = BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE,
        .uuids_complete = { .uuid_cnt = 1, .p_uuids = &adv_uuid },
    };
    ble_advdata_t srdata = {
        .name_type = BLE_ADVDATA_FULL_NAME,
    };
    APP_ERROR_CHECK(ble_advdata_encode(&advdata, s_adv_data.adv_data.p_data,
                                       &s_adv_data.adv_data.len));
    APP_ERROR_CHECK(ble_advdata_encode(&srdata,
                                       s_adv_data.scan_rsp_data.p_data,
                                       &s_adv_data.scan_rsp_data.len));

    ble_gap_adv_params_t params = {
        .properties = { .type =
            BLE_GAP_ADV_TYPE_CONNECTABLE_SCANNABLE_UNDIRECTED },
        .interval   = ADV_INTERVAL,
        .duration   = 0,   /* forever */
        .filter_policy = BLE_GAP_ADV_FP_ANY,
        .primary_phy   = BLE_GAP_PHY_1MBPS,
    };
    APP_ERROR_CHECK(sd_ble_gap_adv_set_configure(&s_adv_handle, &s_adv_data,
                                                  &params));
}

/* ---- Public API -------------------------------------------------------------- */

void ble_ctrl_svc_init(void)
{
    service_init();
    advertising_init();
    NRF_LOG_INFO("ctrl_svc: A6ED service registered, advertising ready");
}

void ble_ctrl_svc_advertise_start(void)
{
    uint32_t err = sd_ble_gap_adv_start(s_adv_handle, CONN_CFG_TAG);
    if (err == NRF_SUCCESS) {
        NRF_LOG_INFO("ctrl_svc: advertising as %s", DEVICE_NAME);
    } else if (err != NRF_ERROR_INVALID_STATE) {   /* already advertising */
        APP_ERROR_CHECK(err);
    }
}

void ble_ctrl_svc_notify(const uint8_t *frame, uint16_t len)
{
    if (len > CTRL_FRAME_MAX) return;
    txq_push(frame, (uint8_t)len);
}
