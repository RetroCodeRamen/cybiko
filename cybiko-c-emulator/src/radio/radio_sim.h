#ifndef CYBIKO_RADIO_SIM_H
#define CYBIKO_RADIO_SIM_H

/*
 * In-process PHY used by host tests. Several co-processors share a bus.
 * Packets are fragmented with the real link codec, then delivered by pump().
 * Nothing here touches a radio or a socket.
 */

#include "radio_link.h"
#include "radio_transport.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RADIO_SIM_MAX_NODES 4
#define RADIO_SIM_INBOX     24

typedef struct radio_sim_bus radio_sim_bus_t;
typedef struct radio_sim_node radio_sim_node_t;

struct radio_sim_bus {
    radio_sim_node_t *nodes[RADIO_SIM_MAX_NODES];
    int count;
    unsigned drop_every;   /* 0 = keep all. 1 = drop every fragment. */
    unsigned seq;
    unsigned hold_pumps;   /* extra pumps before a fragment is visible */
    bool duplicate;        /* deliver each fragment twice */
    unsigned stat_loss;
};

struct radio_sim_node {
    cybiko_radio_transport_t base;
    radio_sim_bus_t *bus;
    size_t phy_mtu;
    uint16_t next_msg_id;
    uint8_t listen_channel;
    cybiko_link_decoder_t dec;
    cybiko_radio_on_packet_fn fn;
    void *fn_user;
    struct {
        bool used;
        unsigned hold;
        uint16_t len;
        uint8_t pkt[CYBIKO_LINK_MAX_PHYS];
    } inbox[RADIO_SIM_INBOX];
};

void radio_sim_bus_init(radio_sim_bus_t *bus);
void radio_sim_node_init(radio_sim_node_t *node, radio_sim_bus_t *bus,
                         uint32_t device_id, size_t phy_mtu);
int  radio_sim_bus_add(radio_sim_bus_t *bus, radio_sim_node_t *node);
void radio_sim_bus_pump(radio_sim_bus_t *bus);

/* Inject one already-encoded PHY packet into a node's inbox. */
int radio_sim_inject(radio_sim_node_t *node, const uint8_t *pkt, size_t len, unsigned hold);

#endif
