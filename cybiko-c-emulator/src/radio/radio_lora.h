#ifndef CYBIKO_RADIO_LORA_H
#define CYBIKO_RADIO_LORA_H

/*
 * cybiko_radio_transport_t over one packet radio (radio_phy.h).
 *
 * send_packet fragments the guest payload with the link codec and queues
 * the PHY packets; it never waits. pump() polls the PHY, starts the next
 * queued packet once the previous one has left the air, and hands every
 * reassembled payload to the listener with the sender's CyID from the
 * link header. A full queue rejects the send (-1) rather than pretending
 * it went out. There is no ACK and no retry: CyOS retransmits itself.
 */

#include "radio_link.h"
#include "radio_phy.h"
#include "radio_transport.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Three CyOS retransmits of a two-fragment frame, plus a beacon. */
#define CYBIKO_LORA_TXQ 8

/* Poll beacons are 42 bytes and ~110 ms on the air at SF7/125 kHz. CyOS
 * (and the Classic driver) wants to send one every second; two or three
 * radios would fill the channel. An unchanged presence beacon (same
 * channel and nickname; the sequence bytes still tick) is held for this
 * many pump() calls. 300 is 5 s at 60 fps. Chat is never held. */
#ifndef CYBIKO_LORA_BEACON_HOLD_PUMPS
#define CYBIKO_LORA_BEACON_HOLD_PUMPS 300
#endif

typedef struct {
    uint32_t tx_msgs;       /* guest payloads queued for the air */
    uint32_t tx_pkts;       /* PHY packets handed to tx() */
    uint32_t tx_rejected;   /* send_packet refused: queue full or too long */
    uint32_t tx_held;       /* unchanged beacons not put on the air */
    uint32_t rx_pkts;       /* PHY packets read */
    uint32_t rx_msgs;       /* payloads delivered to the listener */
    uint32_t rx_held;       /* unchanged beacons not given to CyOS */
    uint32_t rx_bad;        /* malformed or bad link CRC */
} cybiko_lora_stats_t;

typedef struct {
    cybiko_radio_transport_t base;
    cybiko_radio_phy_t *phy;
    uint16_t next_msg_id;
    uint8_t channel;
    bool tx_enabled;
    cybiko_radio_on_packet_fn fn;
    void *fn_user;
    cybiko_link_decoder_t dec;
    struct {
        uint16_t len;
        uint8_t pkt[CYBIKO_LINK_MAX_PHYS];
    } txq[CYBIKO_LORA_TXQ];
    int txq_head, txq_count;
    uint32_t pumps;
    uint32_t last_beacon_pump;
    uint8_t last_beacon_ch;
    uint8_t last_beacon_name[8];
    bool last_beacon_set;
    uint32_t last_rx_beacon_pump;
    uint32_t last_rx_beacon_from;
    uint8_t last_rx_beacon_name[8];
    bool last_rx_beacon_set;
    cybiko_lora_stats_t stats;
} cybiko_lora_t;

/* tx_enabled false: sends are rejected and counted, receive still works.
 * Keep it false until the operator has chosen a legal frequency. */
void cybiko_lora_init(cybiko_lora_t *t, cybiko_radio_phy_t *phy,
                      uint32_t device_id, bool tx_enabled);
void cybiko_lora_set_tx_enabled(cybiko_lora_t *t, bool on);

#ifdef __cplusplus
}
#endif

#endif
