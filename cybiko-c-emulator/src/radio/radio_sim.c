#include "radio_sim.h"

#include <string.h>

void radio_sim_bus_init(radio_sim_bus_t *bus) {
    memset(bus, 0, sizeof *bus);
}

static int sim_start(cybiko_radio_transport_t *self) {
    (void)self;
    return 0;
}

static void sim_close(cybiko_radio_transport_t *self) { (void)self; }

static void sim_set_channel(cybiko_radio_transport_t *self, uint8_t channel) {
    radio_sim_node_t *n = self->impl;
    n->listen_channel = channel;
}

static void sim_set_listener(cybiko_radio_transport_t *self,
                            cybiko_radio_on_packet_fn fn, void *user) {
    radio_sim_node_t *n = self->impl;
    n->fn = fn;
    n->fn_user = user;
}

static int enqueue(radio_sim_node_t *dst, const uint8_t *pkt, size_t len, unsigned hold) {
    if (len > CYBIKO_LINK_MAX_PHYS) return -1;
    for (int i = 0; i < RADIO_SIM_INBOX; i++) {
        if (dst->inbox[i].used) continue;
        dst->inbox[i].used = true;
        dst->inbox[i].hold = hold;
        dst->inbox[i].len = (uint16_t)len;
        memcpy(dst->inbox[i].pkt, pkt, len);
        return 0;
    }
    return -1;
}

int radio_sim_inject(radio_sim_node_t *node, const uint8_t *pkt, size_t len, unsigned hold) {
    return enqueue(node, pkt, len, hold);
}

static void fanout(radio_sim_node_t *src, const uint8_t *pkt, size_t len) {
    radio_sim_bus_t *bus = src->bus;
    for (int i = 0; i < bus->count; i++) {
        radio_sim_node_t *dst = bus->nodes[i];
        if (dst == src) continue;
        bus->seq++;
        if (bus->drop_every && (bus->seq % bus->drop_every) == 0) {
            bus->stat_loss++;
            continue;
        }
        if (enqueue(dst, pkt, len, bus->hold_pumps) != 0)
            bus->stat_loss++;
        if (bus->duplicate) {
            if (enqueue(dst, pkt, len, bus->hold_pumps) != 0)
                bus->stat_loss++;
        }
    }
}

static int sim_send(cybiko_radio_transport_t *self, const uint8_t *data, size_t len, uint8_t channel) {
    radio_sim_node_t *n = self->impl;
    uint8_t pkts[CYBIKO_LINK_MAX_FRAGS][CYBIKO_LINK_MAX_PHYS];
    uint16_t lens[CYBIKO_LINK_MAX_FRAGS];
    int nf = cybiko_link_fragment(n->next_msg_id++, self->device_id, channel,
                                  data, len, n->phy_mtu, pkts, lens, CYBIKO_LINK_MAX_FRAGS);
    if (nf < 0) return -1;
    for (int i = 0; i < nf; i++)
        fanout(n, pkts[i], lens[i]);
    return 0;
}

static void pump_node(radio_sim_node_t *n) {
    int ready[RADIO_SIM_INBOX];
    int nready = 0;
    for (int i = 0; i < RADIO_SIM_INBOX; i++) {
        if (!n->inbox[i].used) continue;
        if (n->inbox[i].hold > 0) {
            n->inbox[i].hold--;
            continue;
        }
        ready[nready++] = i;
    }
    uint8_t out[CYBIKO_LINK_MAX_PAYLOAD];
    for (int r = 0; r < nready; r++) {
        int i = ready[r];
        if (!n->inbox[i].used) continue;
        size_t out_len = 0;
        uint8_t channel = 0;
        uint32_t sender = 0;
        int rc = cybiko_link_push(&n->dec, n->inbox[i].pkt, n->inbox[i].len,
                                  out, sizeof out, &out_len, &channel, &sender);
        n->inbox[i].used = false;
        if (rc == 1 && n->fn)
            n->fn(n->fn_user, out, out_len, channel, sender);
    }
}

static void sim_pump(cybiko_radio_transport_t *self) {
    radio_sim_node_t *n = self->impl;
    if (n->bus)
        radio_sim_bus_pump(n->bus);
}

void radio_sim_bus_pump(radio_sim_bus_t *bus) {
    for (int i = 0; i < bus->count; i++)
        pump_node(bus->nodes[i]);
}

void radio_sim_node_init(radio_sim_node_t *node, radio_sim_bus_t *bus,
                         uint32_t device_id, size_t phy_mtu) {
    memset(node, 0, sizeof *node);
    node->bus = bus;
    node->phy_mtu = phy_mtu ? phy_mtu : CYBIKO_LORA_PHY_MAX;
    node->next_msg_id = 1;
    node->base.device_id = device_id;
    node->base.impl = node;
    node->base.start = sim_start;
    node->base.close = sim_close;
    node->base.send_packet = sim_send;
    node->base.set_channel = sim_set_channel;
    node->base.set_listener = sim_set_listener;
    node->base.pump = sim_pump;
}

int radio_sim_bus_add(radio_sim_bus_t *bus, radio_sim_node_t *node) {
    if (bus->count >= RADIO_SIM_MAX_NODES) return -1;
    bus->nodes[bus->count++] = node;
    node->bus = bus;
    return 0;
}
