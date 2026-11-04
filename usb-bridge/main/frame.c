#include "frame.h"

#include <string.h>

enum {
    ST_MAGIC0 = 0,
    ST_MAGIC1,
    ST_TYPE,
    ST_LEN0,
    ST_LEN1,
    ST_PAYLOAD,
    ST_CRC0,
    ST_CRC1,
};

uint16_t frame_crc16(const uint8_t *data, size_t len, uint16_t crc)
{
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

size_t frame_encode(uint8_t type, const uint8_t *payload, uint16_t len,
                    uint8_t *out, size_t out_size)
{
    if (len > FRAME_MAX_PAYLOAD || out_size < (size_t)len + FRAME_OVERHEAD) {
        return 0;
    }
    out[0] = FRAME_MAGIC0;
    out[1] = FRAME_MAGIC1;
    out[2] = type;
    out[3] = (uint8_t)(len & 0xFF);
    out[4] = (uint8_t)(len >> 8);
    if (len > 0) {
        memcpy(&out[5], payload, len);
    }
    uint16_t crc = frame_crc16(&out[2], (size_t)len + 3, 0xFFFF);
    out[5 + len] = (uint8_t)(crc & 0xFF);
    out[6 + len] = (uint8_t)(crc >> 8);
    return (size_t)len + FRAME_OVERHEAD;
}

void frame_parser_reset(frame_parser_t *p)
{
    p->state = ST_MAGIC0;
    p->len = 0;
    p->pos = 0;
}

static bool crc_ok(const frame_parser_t *p)
{
    uint8_t head[3] = { p->type, (uint8_t)(p->len & 0xFF), (uint8_t)(p->len >> 8) };
    uint16_t crc = frame_crc16(head, sizeof(head), 0xFFFF);
    crc = frame_crc16(p->payload, p->len, crc);
    return crc == p->crc_rx;
}

bool frame_parser_feed(frame_parser_t *p, uint8_t byte)
{
    switch (p->state) {
    case ST_MAGIC0:
        if (byte == FRAME_MAGIC0) {
            p->state = ST_MAGIC1;
        }
        return false;
    case ST_MAGIC1:
        if (byte == FRAME_MAGIC1) {
            p->state = ST_TYPE;
        } else if (byte != FRAME_MAGIC0) {
            p->state = ST_MAGIC0;
        }
        return false;
    case ST_TYPE:
        p->type = byte;
        p->state = ST_LEN0;
        return false;
    case ST_LEN0:
        p->len = byte;
        p->state = ST_LEN1;
        return false;
    case ST_LEN1:
        p->len |= (uint16_t)byte << 8;
        if (p->len > FRAME_MAX_PAYLOAD) {
            frame_parser_reset(p);
            return false;
        }
        p->pos = 0;
        p->state = (p->len == 0) ? ST_CRC0 : ST_PAYLOAD;
        return false;
    case ST_PAYLOAD:
        p->payload[p->pos++] = byte;
        if (p->pos == p->len) {
            p->state = ST_CRC0;
        }
        return false;
    case ST_CRC0:
        p->crc_rx = byte;
        p->state = ST_CRC1;
        return false;
    case ST_CRC1:
        p->crc_rx |= (uint16_t)byte << 8;
        p->state = ST_MAGIC0;
        return crc_ok(p);
    default:
        frame_parser_reset(p);
        return false;
    }
}
