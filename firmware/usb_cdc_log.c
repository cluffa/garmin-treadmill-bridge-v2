/*
 * usb_cdc_log.c — USB-CDC ACM console: log backend + interactive ctrl dispatch.
 *
 * Implements a CDC ACM virtual serial port over the nRF52840 USB peripheral.
 * Received bytes are assembled into lines; each complete line is forwarded to
 * the weak usb_cdc_on_line() hook. The default hook dispatches the line to
 * core/ctrl_dispatch() and writes any reply lines back to CDC.
 *
 * TX is non-blocking: if the endpoint is busy or the port is not open, data
 * is silently dropped.
 */

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "app_error.h"
#include "app_usbd.h"
#include "app_usbd_cdc_acm.h"
#include "app_usbd_core.h"
#include "app_usbd_serial_num.h"
#include "app_usbd_string_desc.h"
#include "app_util_platform.h"
#include "nrf_drv_power.h"
#include "nrf_drv_usbd.h"
#include "nrf_log.h"
#include "nrf_log_ctrl.h"
#include "nrf_log_backend_interface.h"
#include "nrf_log_backend_serial.h"

#include "ctrl_dispatch.h"
#include "usb_cdc_log.h"

/* ---- CDC ACM endpoint / interface config ---------------------------------- */

#define CDC_ACM_COMM_INTERFACE  0
#define CDC_ACM_COMM_EPIN       NRF_DRV_USBD_EPIN2
#define CDC_ACM_DATA_INTERFACE  1
#define CDC_ACM_DATA_EPIN       NRF_DRV_USBD_EPIN1
#define CDC_ACM_DATA_EPOUT      NRF_DRV_USBD_EPOUT1

/* ---- Line reader ---------------------------------------------------------- */

#define LINE_BUF_SIZE 128

static char   s_line_buf[LINE_BUF_SIZE];
static size_t s_line_pos;

/* ---- RX buffer ------------------------------------------------------------ */

#define CDC_READ_SIZE 64

static char s_rx_buf[CDC_READ_SIZE];

/* ---- TX ring buffer ------------------------------------------------------- */

/*
 * app_usbd_cdc_acm_write() builds a transfer descriptor that points into the
 * caller's buffer and queues it for asynchronous EasyDMA.  The buffer MUST
 * outlive the call.  A stack buffer is UB and silently corrupts USB output.
 *
 * usb_cdc_log_write() is called from both IRQ (app_timer -> heartbeat_cb) and
 * thread (cdc_tx_sink) context, so the two can preempt each other.  A single
 * static buffer is therefore not enough either — the IRQ path could overwrite
 * it while a thread-mode transfer is still in flight.
 *
 * Solution: a small static ring of endpoint-sized buffers.  The write side
 * claims a free slot under critical-section protection (soft-irq-safe);
 * APP_USBD_CDC_ACM_USER_EVT_TX_DONE releases it.  If no slot is free the
 * message is dropped (with a diagnostic counter) — blocking in IRQ context
 * would stall the SoftDevice event dispatch.
 */

#define CDC_TX_RING_SIZE 4  /* power of two; 4 * 64 = 256 B */

static char      s_cdc_tx_ring[CDC_TX_RING_SIZE][NRF_DRV_USBD_EPSIZE];
static volatile uint8_t  s_cdc_tx_wr;   /* next slot to claim              */
static volatile uint8_t  s_cdc_tx_rd;   /* next slot TX_DONE will release  */
static volatile uint8_t  s_cdc_tx_cnt;  /* outstanding transfers (0..N)    */
static volatile uint32_t s_cdc_tx_drops;/* diagnostic: messages dropped    */

/* ---- Forward declarations ------------------------------------------------- */

static void cdc_acm_user_ev_handler(app_usbd_class_inst_t const *p_inst,
                                    app_usbd_cdc_acm_user_event_t event);

static void cdc_tx_raw(const uint8_t *data, size_t len);

/* ---- CDC ACM class instance ----------------------------------------------- */

APP_USBD_CDC_ACM_GLOBAL_DEF(m_cdc_acm,
                            cdc_acm_user_ev_handler,
                            CDC_ACM_COMM_INTERFACE,
                            CDC_ACM_DATA_INTERFACE,
                            CDC_ACM_COMM_EPIN,
                            CDC_ACM_DATA_EPIN,
                            CDC_ACM_DATA_EPOUT,
                            APP_USBD_CDC_COMM_PROTOCOL_AT_V250);

/* ---- TX ring helper (shared by usb_cdc_log_write + log backend) ----------- */

static void cdc_tx_raw(const uint8_t *data, size_t len)
{
    if (len == 0) return;

    /* One endpoint packet max — longer messages truncate. */
    if (len > NRF_DRV_USBD_EPSIZE) {
        len = NRF_DRV_USBD_EPSIZE;
    }

    /*
     * Claim a ring slot, copy the payload, and queue the transfer.
     *
     * The whole sequence is inside the critical region: if another writer
     * slipped in between a failed write and the un-claim rollback, we would
     * rewind over its slot — exactly the corruption this ring prevents.
     */
    CRITICAL_REGION_ENTER();
    if (s_cdc_tx_cnt >= CDC_TX_RING_SIZE) {
        s_cdc_tx_drops++;
    } else {
        uint8_t slot = s_cdc_tx_wr;
        char   *buf  = s_cdc_tx_ring[slot];

        memcpy(buf, data, len);

        s_cdc_tx_wr = (uint8_t)((s_cdc_tx_wr + 1) % CDC_TX_RING_SIZE);
        s_cdc_tx_cnt++;

        if (app_usbd_cdc_acm_write(&m_cdc_acm, buf, len) != NRF_SUCCESS) {
            s_cdc_tx_wr = slot;
            s_cdc_tx_cnt--;
            s_cdc_tx_drops++;
        }
    }
    CRITICAL_REGION_EXIT();
}

/* ---- NRF_LOG backend over CDC ACM ---------------------------------------- */

#define CDC_LOG_BUF_SIZE 64

static uint8_t s_log_buf[CDC_LOG_BUF_SIZE];

static void cdc_serial_tx(void const *p_context, char const *p_buffer, size_t len)
{
    (void)p_context;
    cdc_tx_raw((const uint8_t *)p_buffer, len);
}

static void cdc_log_put(nrf_log_backend_t const *p_backend,
                        nrf_log_entry_t *p_msg)
{
    /*
     * NRF_LOG_DEFERRED is 0, so this runs synchronously in whatever context
     * called NRF_LOG_*: thread mode (main loop, cdc_tx_sink) AND IRQ priority 6
     * (SoftDevice event handlers, app_timer callbacks). s_log_buf is a single
     * shared formatting buffer, so a thread-mode log that gets preempted by an
     * IRQ-context log would have its half-formatted line overwritten.
     *
     * Serialise the format-and-queue. Two prio-6 IRQs cannot preempt each
     * other, so this is really about thread-vs-IRQ. The region covers only
     * formatting one <=64 byte line plus the ring push (which nests its own
     * critical region — supported).
     */
    CRITICAL_REGION_ENTER();
    nrf_log_backend_serial_put(p_backend, p_msg, s_log_buf,
                               CDC_LOG_BUF_SIZE, cdc_serial_tx);
    CRITICAL_REGION_EXIT();
}

static void cdc_log_flush(nrf_log_backend_t const *p_backend)
{
    (void)p_backend;
    /*
     * USB TX is asynchronous (EasyDMA).  In normal operation the ring
     * drains via TX_DONE events.  In panic mode USB interrupts may not
     * fire, so spinning here would deadlock.  The backend is best-effort.
     */
}

static void cdc_log_panic_set(nrf_log_backend_t const *p_backend)
{
    (void)p_backend;
    /*
     * USB CDC ACM cannot be reconfigured to blocking mode — all transfers
     * go through EasyDMA.  The ring path remains non-blocking; queued
     * log lines may never complete if USB interrupts are stopped during
     * the panic handler.
     */
}

static const nrf_log_backend_api_t cdc_log_backend_api = {
    .put       = cdc_log_put,
    .flush     = cdc_log_flush,
    .panic_set = cdc_log_panic_set,
};

static nrf_log_backend_cb_t cdc_log_backend_cb = {
    .enabled = false,
    .id      = NRF_LOG_BACKEND_INVALID_ID,
    .p_next  = NULL
};

static const nrf_log_backend_t cdc_log_backend = {
    .p_api  = &cdc_log_backend_api,
    .p_ctx  = NULL,
    .p_cb   = &cdc_log_backend_cb,
    .p_name = "cdc_log_backend"
};

/* ---- TX sink for ctrl_dispatch -------------------------------------------- */

static void cdc_tx_sink(const char *msg, void *ctx)
{
    (void)ctx;
    usb_cdc_log_write(msg);
}

/* ---- Line processing ------------------------------------------------------ */

static void process_line(void)
{
    if (s_line_pos == 0) return;

    s_line_buf[s_line_pos] = '\0';
    s_line_pos = 0;

    usb_cdc_on_line(s_line_buf);
}

static void feed_line_reader(const char *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        char c = data[i];

        if (c == '\r' || c == '\n') {
            process_line();
            continue;
        }

        if (s_line_pos < LINE_BUF_SIZE - 1) {
            s_line_buf[s_line_pos++] = c;
        }
    }
}

/* ---- CDC ACM user events -------------------------------------------------- */

static void cdc_acm_user_ev_handler(app_usbd_class_inst_t const *p_inst,
                                    app_usbd_cdc_acm_user_event_t event)
{
    app_usbd_cdc_acm_t const *p_cdc_acm = app_usbd_cdc_acm_class_get(p_inst);

    switch (event) {
    case APP_USBD_CDC_ACM_USER_EVT_PORT_OPEN:
        NRF_LOG_INFO("CDC ACM port opened");

        /* Reset the TX ring — any outstanding transfers are stale. */
        CRITICAL_REGION_ENTER();
        s_cdc_tx_wr   = 0;
        s_cdc_tx_rd   = 0;
        s_cdc_tx_cnt  = 0;
        s_cdc_tx_drops = 0;
        CRITICAL_REGION_EXIT();

        /* Arm the first read. Must be read_any(), NOT read(): read() only
         * raises RX_DONE once the *full* requested length has accumulated, so a
         * short interactive line like "STATUS\n" (7 B) would never be delivered
         * until 64 B piled up. read_any() delivers whatever has arrived. */
        (void)app_usbd_cdc_acm_read_any(&m_cdc_acm, s_rx_buf, sizeof(s_rx_buf));
        break;

    case APP_USBD_CDC_ACM_USER_EVT_PORT_CLOSE:
        NRF_LOG_INFO("CDC ACM port closed");
        break;

    case APP_USBD_CDC_ACM_USER_EVT_TX_DONE:
        /* Release the oldest outstanding TX buffer back to the ring.
         * Transfers complete in FIFO order on a single IN endpoint so
         * simply advancing s_cdc_tx_rd is correct. */
        CRITICAL_REGION_ENTER();
        if (s_cdc_tx_cnt > 0) {
            s_cdc_tx_cnt--;
            s_cdc_tx_rd = (uint8_t)(s_cdc_tx_rd + 1) % CDC_TX_RING_SIZE;
        }
        CRITICAL_REGION_EXIT();
        break;

    case APP_USBD_CDC_ACM_USER_EVT_RX_DONE: {
        /* Drain loop: process the completed read, then re-arm with read_any().
         * When read_any() returns NRF_SUCCESS the next chunk is already in the
         * internal buffer (no further RX_DONE will fire for it), so we must loop
         * and process it here; NRF_ERROR_IO_PENDING means "wait for RX_DONE". */
        ret_code_t ret;
        do {
            size_t avail = app_usbd_cdc_acm_rx_size(p_cdc_acm);
            if (avail > 0 && avail <= sizeof(s_rx_buf)) {
                feed_line_reader(s_rx_buf, avail);
            }
            ret = app_usbd_cdc_acm_read_any(&m_cdc_acm, s_rx_buf,
                                            sizeof(s_rx_buf));
        } while (ret == NRF_SUCCESS);
        break;
    }

    default:
        break;
    }
}

/* ---- USBD state events ---------------------------------------------------- */

static void usbd_user_ev_handler(app_usbd_event_type_t event)
{
    switch (event) {
    case APP_USBD_EVT_DRV_SUSPEND:
        break;
    case APP_USBD_EVT_DRV_RESUME:
        break;
    case APP_USBD_EVT_STARTED:
        NRF_LOG_INFO("USBD started");
        break;
    case APP_USBD_EVT_STOPPED:
        app_usbd_disable();
        break;
    case APP_USBD_EVT_POWER_DETECTED:
        NRF_LOG_INFO("USB power detected");
        if (!nrf_drv_usbd_is_enabled()) {
            app_usbd_enable();
        }
        break;
    case APP_USBD_EVT_POWER_REMOVED:
        NRF_LOG_INFO("USB power removed");
        app_usbd_stop();
        break;
    case APP_USBD_EVT_POWER_READY:
        NRF_LOG_INFO("USB ready");
        app_usbd_start();
        break;
    default:
        break;
    }
}

/* ---- Public API ----------------------------------------------------------- */

void usb_cdc_log_init(void)
{
    static const app_usbd_config_t usbd_config = {
        .ev_state_proc = usbd_user_ev_handler
    };

    app_usbd_serial_num_generate();

    ret_code_t ret = app_usbd_init(&usbd_config);
    APP_ERROR_CHECK(ret);

    app_usbd_class_inst_t const *class_cdc_acm =
        app_usbd_cdc_acm_class_inst_get(&m_cdc_acm);
    ret = app_usbd_class_append(class_cdc_acm);
    APP_ERROR_CHECK(ret);

    ret = app_usbd_power_events_enable();
    APP_ERROR_CHECK(ret);

    /*
     * Register the CDC ACM NRF_LOG backend so that NRF_LOG_INFO / WARNING /
     * ERROR calls reach USB in addition to RTT.  Registration happens after
     * USB is initialised so that app_usbd_cdc_acm_write() is functional when
     * the first log message arrives.
     */
    int32_t backend_id = nrf_log_backend_add(&cdc_log_backend,
                                             NRF_LOG_SEVERITY_DEBUG);
    if (backend_id >= 0) {
        nrf_log_backend_enable(&cdc_log_backend);
    }
}

void usb_cdc_log_write(const char *msg)
{
    if (!msg) return;

    size_t len = strlen(msg);
    if (len == 0) return;

    /* Truncate to what fits in one endpoint packet (minus CR+LF). */
    if (len > NRF_DRV_USBD_EPSIZE - 2) {
        len = NRF_DRV_USBD_EPSIZE - 2;
    }

    /*
     * Build the CR+LF-terminated payload on the stack, then push it
     * through cdc_tx_raw().  The double-copy is intentional — the ring
     * buffer must own every buffer passed to app_usbd_cdc_acm_write()
     * (EasyDMA), and stack buffers are UB there.
     */
    char buf[NRF_DRV_USBD_EPSIZE];
    memcpy(buf, msg, len);
    buf[len]     = '\r';
    buf[len + 1] = '\n';

    cdc_tx_raw((const uint8_t *)buf, len + 2);
}

/* ---- Weak hook (overridable by application) ------------------------------- */

__attribute__((weak))
void usb_cdc_on_line(const char *line)
{
    if (!line || line[0] == '\0') return;

    ctrl_dispatch(line, cdc_tx_sink, NULL);
}
