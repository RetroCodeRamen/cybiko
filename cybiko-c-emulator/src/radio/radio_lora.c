#include "radio_lora.h"

#include <string.h>

static int lora_start(cybiko_radio_transport_t *self) {
    (void)self;
    return 0;
}

static void lora_close(cybiko_radio_transport_t *self) { (void)self; }

static void lora_set_channel(cybiko_radio_transport_t *self, uint8_t channel) {
    cybiko_lora_t *t = self->impl;
    t->channel = channel;
}

static void lora_set_listener(cybiko_radio_transport_t *self,
                              cybiko_radio_on_packet_fn fn, void *user) {
    cybiko_lora_t *t = self->impl;
    t->fn = fn;
    t->fn_user = user;
}

/* Guest payload after the 8-byte RF header is stripped: byte 0 is
 * 0xC0|channel, byte 1 is the frame type (0x01 = presence beacon),
 * bytes 12-19 are the nickname. Sequence/CRC bytes still change. */
static bool lora_is_beacon(const uint8_t *data, size_t len) {
    return data && len >= 20 && len <= 50 && data[1] == 0x01;
}

static int lora_send(cybiko_radio_transport_t *self,
                     const uint8_t *data, size_t len, uint8_t channel) {
    cybiko_lora_t *t = self->impl;
    if (!t->tx_enabled) {
        t->stats.tx_rejected++;
        return -1;
    }
    if (lora_is_beacon(data, len) && t->last_beacon_set &&
        data[0] == t->last_beacon_ch &&
        memcmp(data + 12, t->last_beacon_name, 8) == 0 &&
        t->pumps - t->last_beacon_pump < CYBIKO_LORA_BEACON_HOLD_PUMPS) {
        t->stats.tx_held++;
        return 0;   /* CyOS already transmitted; keep it off the air */
    }
    uint8_t pkts[CYBIKO_LINK_MAX_FRAGS][CYBIKO_LINK_MAX_PHYS];
    uint16_t lens[CYBIKO_LINK_MAX_FRAGS];
    int nf = cybiko_link_fragment(t->next_msg_id, self->device_id, channel,
                                  data, len, t->phy->mtu, pkts, lens,
                                  CYBIKO_LINK_MAX_FRAGS);
    /* All fragments or none: half a message only wastes airtime. */
    if (nf < 0 || nf > CYBIKO_LORA_TXQ - t->txq_count) {
        t->stats.tx_rejected++;
        return -1;
    }
    t->next_msg_id++;
    for (int i = 0; i < nf; i++) {
        int slot = (t->txq_head + t->txq_count) % CYBIKO_LORA_TXQ;
        t->txq[slot].len = lens[i];
        memcpy(t->txq[slot].pkt, pkts[i], lens[i]);
        t->txq_count++;
    }
    t->stats.tx_msgs++;
    if (lora_is_beacon(data, len)) {
        t->last_beacon_set = true;
        t->last_beacon_ch = data[0];
        memcpy(t->last_beacon_name, data + 12, 8);
        t->last_beacon_pump = t->pumps;
    }
    return 0;
}

static void lora_pump(cybiko_radio_transport_t *self) {
    cybiko_lora_t *t = self->impl;
    cybiko_radio_phy_t *phy = t->phy;
    t->pumps++;
    if (phy->poll)
        phy->poll(phy);

    uint8_t pkt[CYBIKO_LINK_MAX_PHYS];
    uint8_t out[CYBIKO_LINK_MAX_PAYLOAD];
    size_t len;
    while (phy->rx(phy, pkt, sizeof pkt, &len) == 1) {
        t->stats.rx_pkts++;
        size_t out_len = 0;
        uint8_t channel = 0;
        uint32_t sender = 0;
        int rc = cybiko_link_push(&t->dec, pkt, len, out, sizeof out,
                                  &out_len, &channel, &sender);
        if (rc < 0) {
            t->stats.rx_bad++;
        } else if (rc == 1) {
            if (lora_is_beacon(out, out_len) && t->last_rx_beacon_set &&
                sender == t->last_rx_beacon_from &&
                memcmp(out + 12, t->last_rx_beacon_name, 8) == 0 &&
                t->pumps - t->last_rx_beacon_pump < CYBIKO_LORA_BEACON_HOLD_PUMPS) {
                t->stats.rx_held++;
                continue;
            }
            t->stats.rx_msgs++;
            if (lora_is_beacon(out, out_len)) {
                t->last_rx_beacon_set = true;
                t->last_rx_beacon_from = sender;
                memcpy(t->last_rx_beacon_name, out + 12, 8);
                t->last_rx_beacon_pump = t->pumps;
            }
            if (t->fn)
                t->fn(t->fn_user, out, out_len, channel, sender);
        }
    }

    if (t->txq_count > 0 && !phy->tx_busy(phy)) {
        if (phy->tx(phy, t->txq[t->txq_head].pkt, t->txq[t->txq_head].len) == 0) {
            t->txq_head = (t->txq_head + 1) % CYBIKO_LORA_TXQ;
            t->txq_count--;
            t->stats.tx_pkts++;
        }
    }
}

void cybiko_lora_init(cybiko_lora_t *t, cybiko_radio_phy_t *phy,
                      uint32_t device_id, bool tx_enabled) {
    memset(t, 0, sizeof *t);
    t->phy = phy;
    t->next_msg_id = 1;
    t->tx_enabled = tx_enabled;
    t->base.device_id = device_id;
    t->base.impl = t;
    t->base.start = lora_start;
    t->base.close = lora_close;
    t->base.send_packet = lora_send;
    t->base.set_channel = lora_set_channel;
    t->base.set_listener = lora_set_listener;
    t->base.pump = lora_pump;
}

void cybiko_lora_set_tx_enabled(cybiko_lora_t *t, bool on) {
    t->tx_enabled = on;
}
