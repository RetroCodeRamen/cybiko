#include "radio_link.h"

#include <string.h>

uint16_t cybiko_link_crc16(const uint8_t *data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int bit = 0; bit < 8; bit++) {
            if (crc & 0x8000)
                crc = (uint16_t)((crc << 1) ^ 0x1021);
            else
                crc = (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static uint16_t crc_header_and_chunk(const uint8_t *hdr, const uint8_t *chunk, size_t chunk_len) {
    uint8_t scratch[CYBIKO_LINK_HDR + (CYBIKO_LINK_MAX_PHYS - CYBIKO_LINK_HDR)];
    memcpy(scratch, hdr, 14);
    scratch[14] = 0;
    scratch[15] = 0;
    if (chunk_len)
        memcpy(scratch + CYBIKO_LINK_HDR, chunk, chunk_len);
    return cybiko_link_crc16(scratch, CYBIKO_LINK_HDR + chunk_len);
}

static void write_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static uint16_t read_u16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

static uint32_t read_u32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

int cybiko_link_fragment(uint16_t msg_id, uint32_t sender_id, uint8_t channel,
                         const uint8_t *payload, size_t len, size_t phy_mtu,
                         uint8_t out[][CYBIKO_LINK_MAX_PHYS],
                         uint16_t *out_len, int max_out) {
    if (!payload || !out || !out_len) return -1;
    if (phy_mtu <= CYBIKO_LINK_HDR || phy_mtu > CYBIKO_LINK_MAX_PHYS) return -1;
    if (len == 0 || len > CYBIKO_LINK_MAX_PAYLOAD) return -1;

    size_t chunk_cap = phy_mtu - CYBIKO_LINK_HDR;
    int frags = (int)((len + chunk_cap - 1) / chunk_cap);
    if (frags < 1 || frags > CYBIKO_LINK_MAX_FRAGS || frags > max_out) return -1;

    size_t offset = 0;
    for (int i = 0; i < frags; i++) {
        size_t n = len - offset;
        if (n > chunk_cap) n = chunk_cap;
        uint8_t *pkt = out[i];
        memset(pkt, 0, CYBIKO_LINK_HDR);
        pkt[0] = CYBIKO_LINK_MAGIC;
        pkt[1] = CYBIKO_LINK_VERSION;
        pkt[2] = 0;
        write_u16(pkt + 3, msg_id);
        pkt[5] = (uint8_t)i;
        pkt[6] = (uint8_t)frags;
        pkt[7] = channel;
        pkt[8] = (uint8_t)(sender_id >> 24);
        pkt[9] = (uint8_t)(sender_id >> 16);
        pkt[10] = (uint8_t)(sender_id >> 8);
        pkt[11] = (uint8_t)sender_id;
        write_u16(pkt + 12, (uint16_t)len);
        uint16_t crc = crc_header_and_chunk(pkt, payload + offset, n);
        write_u16(pkt + 14, crc);
        memcpy(pkt + CYBIKO_LINK_HDR, payload + offset, n);
        out_len[i] = (uint16_t)(CYBIKO_LINK_HDR + n);
        offset += n;
    }
    return frags;
}

static bool crc_ok(const uint8_t *pkt, size_t len) {
    uint16_t expect = read_u16(pkt + 14);
    uint16_t got = crc_header_and_chunk(pkt, pkt + CYBIKO_LINK_HDR, len - CYBIKO_LINK_HDR);
    return got == expect;
}

static bool dup_seen(cybiko_link_dup_t *d, uint16_t id) {
    for (uint8_t i = 0; i < d->count; i++) {
        if (d->ids[i] == id) return true;
    }
    return false;
}

static void dup_add(cybiko_link_dup_t *d, uint16_t id) {
    if (d->count < CYBIKO_LINK_DUP_WINDOW) {
        d->ids[d->count++] = id;
        return;
    }
    memmove(d->ids, d->ids + 1, (CYBIKO_LINK_DUP_WINDOW - 1) * sizeof(uint16_t));
    d->ids[CYBIKO_LINK_DUP_WINDOW - 1] = id;
}

static cybiko_link_dup_t *dup_slot(cybiko_link_decoder_t *dec, uint32_t sender, bool create) {
    cybiko_link_dup_t *free_slot = NULL;
    for (int i = 0; i < CYBIKO_LINK_DUP_SENDERS; i++) {
        if (dec->dups[i].used && dec->dups[i].sender == sender)
            return &dec->dups[i];
        if (!dec->dups[i].used && !free_slot)
            free_slot = &dec->dups[i];
    }
    if (!create) return NULL;
    if (!free_slot) {
        /* Forget the oldest tracked sender. Beacons are single-fragment,
         * so losing dup state only risks one extra delivery. */
        free_slot = &dec->dups[0];
        memmove(&dec->dups[0], &dec->dups[1],
                (CYBIKO_LINK_DUP_SENDERS - 1) * sizeof(dec->dups[0]));
        memset(&dec->dups[CYBIKO_LINK_DUP_SENDERS - 1], 0, sizeof(dec->dups[0]));
        free_slot = &dec->dups[CYBIKO_LINK_DUP_SENDERS - 1];
    }
    memset(free_slot, 0, sizeof *free_slot);
    free_slot->used = true;
    free_slot->sender = sender;
    return free_slot;
}

static void slot_reset(cybiko_link_slot_t *s, uint32_t sender, uint16_t msg_id,
                       uint8_t channel, uint8_t frag_count, uint16_t total) {
    memset(s, 0, sizeof *s);
    s->active = true;
    s->sender = sender;
    s->msg_id = msg_id;
    s->channel = channel;
    s->frag_count = frag_count;
    s->total_len = total;
}

int cybiko_link_push(cybiko_link_decoder_t *dec,
                     const uint8_t *pkt, size_t len,
                     uint8_t *out, size_t out_cap, size_t *out_len,
                     uint8_t *channel, uint32_t *sender_id) {
    if (!dec || !pkt || !out || !out_len || !channel || !sender_id) return -1;
    if (len < CYBIKO_LINK_HDR || len > CYBIKO_LINK_MAX_PHYS) {
        dec->stat_malformed++;
        return -1;
    }
    if (pkt[0] != CYBIKO_LINK_MAGIC || pkt[1] != CYBIKO_LINK_VERSION) {
        dec->stat_malformed++;
        return -1;
    }
    if (!crc_ok(pkt, len)) {
        dec->stat_crc++;
        return -1;
    }

    uint16_t msg_id = read_u16(pkt + 3);
    uint8_t frag_index = pkt[5];
    uint8_t frag_count = pkt[6];
    uint8_t ch = pkt[7];
    uint32_t sender = read_u32(pkt + 8);
    uint16_t total = read_u16(pkt + 12);
    size_t chunk_len = len - CYBIKO_LINK_HDR;

    if (frag_count == 0 || frag_count > CYBIKO_LINK_MAX_FRAGS || frag_index >= frag_count ||
        total == 0 || total > CYBIKO_LINK_MAX_PAYLOAD || chunk_len == 0) {
        dec->stat_malformed++;
        return -1;
    }

    cybiko_link_dup_t *dup = dup_slot(dec, sender, true);
    if (dup && dup_seen(dup, msg_id)) {
        dec->stat_dup++;
        return 0;
    }

    if (frag_count == 1) {
        if (frag_index != 0 || chunk_len != total || out_cap < total) {
            dec->stat_drop++;
            return -1;
        }
        memcpy(out, pkt + CYBIKO_LINK_HDR, total);
        *out_len = total;
        *channel = ch;
        *sender_id = sender;
        dup_add(dup, msg_id);
        dec->stat_ok++;
        return 1;
    }

    cybiko_link_slot_t *slot = NULL;
    cybiko_link_slot_t *empty = NULL;
    for (int i = 0; i < CYBIKO_LINK_SLOTS; i++) {
        cybiko_link_slot_t *s = &dec->slots[i];
        if (s->active && s->sender == sender && s->msg_id == msg_id) {
            slot = s;
            break;
        }
        if (!s->active && !empty) empty = s;
    }
    if (!slot) {
        /* A new id from a sender replaces that sender's half-built message. */
        for (int i = 0; i < CYBIKO_LINK_SLOTS && !empty; i++) {
            if (dec->slots[i].active && dec->slots[i].sender == sender) {
                dec->stat_drop++;
                empty = &dec->slots[i];
            }
        }
        if (!empty) {
            dec->stat_drop++;
            return 0;
        }
        slot_reset(empty, sender, msg_id, ch, frag_count, total);
        slot = empty;
    }
    if (slot->frag_count != frag_count || slot->total_len != total || slot->channel != ch) {
        dec->stat_drop++;
        slot->active = false;
        return -1;
    }
    if (slot->got & (uint8_t)(1u << frag_index))
        return 0;
    if (chunk_len > sizeof slot->chunks[0]) {
        dec->stat_drop++;
        slot->active = false;
        return -1;
    }
    memcpy(slot->chunks[frag_index], pkt + CYBIKO_LINK_HDR, chunk_len);
    slot->chunk_len[frag_index] = (uint16_t)chunk_len;
    slot->got = (uint8_t)(slot->got | (uint8_t)(1u << frag_index));

    uint8_t need = (uint8_t)((1u << frag_count) - 1u);
    if (slot->got != need) return 0;

    size_t sum = 0;
    for (uint8_t i = 0; i < frag_count; i++)
        sum += slot->chunk_len[i];
    if (sum != total || out_cap < total) {
        dec->stat_drop++;
        slot->active = false;
        return -1;
    }
    size_t at = 0;
    for (uint8_t i = 0; i < frag_count; i++) {
        memcpy(out + at, slot->chunks[i], slot->chunk_len[i]);
        at += slot->chunk_len[i];
    }
    *out_len = total;
    *channel = ch;
    *sender_id = sender;
    slot->active = false;
    dup_add(dup, msg_id);
    dec->stat_ok++;
    return 1;
}

int cybiko_link_self_check(void) {
    static cybiko_link_decoder_t dec;
    static uint8_t pkts[CYBIKO_LINK_MAX_FRAGS][CYBIKO_LINK_MAX_PHYS];
    static uint8_t payload[100];
    static uint8_t out[CYBIKO_LINK_MAX_PAYLOAD];
    static uint16_t lens[CYBIKO_LINK_MAX_FRAGS];

    memset(&dec, 0, sizeof dec);
    for (int i = 0; i < 100; i++)
        payload[i] = (uint8_t)(i + 1);

    int n = cybiko_link_fragment(7, 0x01020304u, 2, payload, 100, 64,
                                 pkts, lens, CYBIKO_LINK_MAX_FRAGS);
    if (n != 3) return -1;

    size_t out_len = 0;
    uint8_t ch = 0;
    uint32_t sender = 0;
    /* Deliver the last fragment first, then the rest, out of order. */
    int order[3] = {2, 0, 1};
    int ready = 0;
    for (int i = 0; i < 3; i++) {
        int rc = cybiko_link_push(&dec, pkts[order[i]], lens[order[i]],
                                  out, sizeof out, &out_len, &ch, &sender);
        if (i < 2) {
            if (rc != 0) return -2;
        } else {
            ready = rc;
        }
    }
    if (ready != 1 || out_len != 100 || ch != 2 || sender != 0x01020304u) return -3;
    if (memcmp(out, payload, 100) != 0) return -4;

    int dup = cybiko_link_push(&dec, pkts[0], lens[0], out, sizeof out, &out_len, &ch, &sender);
    if (dup != 0 || dec.stat_dup < 1) return -5;

    pkts[0][lens[0] - 1] ^= 0xFF;
    int bad = cybiko_link_push(&dec, pkts[0], lens[0], out, sizeof out, &out_len, &ch, &sender);
    if (bad != -1 || dec.stat_crc < 1) return -6;
    return 0;
}
