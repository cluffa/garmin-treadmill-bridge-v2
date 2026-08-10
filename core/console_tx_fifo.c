/* console_tx_fifo.c — see header for the design and the drop-accounting rule. */

#include "console_tx_fifo.h"

void console_tx_fifo_init(console_tx_fifo_t *f, uint8_t *buf, uint16_t size)
{
    f->buf       = buf;
    f->size      = size;
    f->head      = 0;
    f->tail      = 0;
    f->port_open = false;
    f->drops     = 0;
}

void console_tx_fifo_push(console_tx_fifo_t *f, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        uint16_t next = (uint16_t)((f->head + 1u) & (f->size - 1u));
        if (next == f->tail) {          /* full — drop the rest of the message */
            if (f->port_open) {
                f->drops += (uint32_t)(len - i);
            }
            return;
        }
        f->buf[f->head] = data[i];
        f->head = next;
    }
}

size_t console_tx_fifo_pop(console_tx_fifo_t *f, uint8_t *dst, size_t max)
{
    size_t n = 0;
    while (n < max && f->tail != f->head) {
        dst[n++] = f->buf[f->tail];
        f->tail = (uint16_t)((f->tail + 1u) & (f->size - 1u));
    }
    return n;
}

void console_tx_fifo_note_refused(console_tx_fifo_t *f, size_t len)
{
    if (f->port_open) {
        f->drops += (uint32_t)len;
    }
}

void console_tx_fifo_port_event(console_tx_fifo_t *f, bool open)
{
    f->port_open = open;
    if (open) {
        /* Whatever is queued predates the attach; the listener never asked for
         * it and the drop rule says it does not count. The counter survives. */
        f->head = 0;
        f->tail = 0;
    }
}

uint16_t console_tx_fifo_pending(const console_tx_fifo_t *f)
{
    return (uint16_t)((f->head - f->tail) & (f->size - 1u));
}

uint32_t console_tx_fifo_drops(const console_tx_fifo_t *f)
{
    return f->drops;
}
