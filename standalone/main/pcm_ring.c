#include "pcm_ring.h"

#include <stdlib.h>
#include <string.h>

bool pcm_ring_init(pcm_ring_t *r, size_t cap)
{
    r->buf = malloc(cap);
    r->cap = cap;
    r->head = 0;
    r->count = 0;
    portMUX_INITIALIZE(&r->lock);
    return r->buf != NULL;
}

size_t pcm_ring_write(pcm_ring_t *r, const uint8_t *data, size_t len)
{
    taskENTER_CRITICAL(&r->lock);
    size_t space = r->cap - r->count;
    size_t n = len < space ? len : space;
    size_t tail = (r->head + r->count) % r->cap;
    size_t first = n < r->cap - tail ? n : r->cap - tail;
    memcpy(r->buf + tail, data, first);
    memcpy(r->buf, data + first, n - first);
    r->count += n;
    taskEXIT_CRITICAL(&r->lock);
    return n;
}

size_t pcm_ring_read(pcm_ring_t *r, uint8_t *out, size_t len)
{
    taskENTER_CRITICAL(&r->lock);
    size_t n = len < r->count ? len : r->count;
    size_t first = n < r->cap - r->head ? n : r->cap - r->head;
    memcpy(out, r->buf + r->head, first);
    memcpy(out + first, r->buf, n - first);
    r->head = (r->head + n) % r->cap;
    r->count -= n;
    taskEXIT_CRITICAL(&r->lock);
    return n;
}

size_t pcm_ring_count(pcm_ring_t *r)
{
    taskENTER_CRITICAL(&r->lock);
    size_t n = r->count;
    taskEXIT_CRITICAL(&r->lock);
    return n;
}

void pcm_ring_clear(pcm_ring_t *r)
{
    taskENTER_CRITICAL(&r->lock);
    r->head = 0;
    r->count = 0;
    taskEXIT_CRITICAL(&r->lock);
}
