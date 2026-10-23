#ifndef CYBIKO_RADIO_PHY_H
#define CYBIKO_RADIO_PHY_H

/*
 * One raw packet radio under radio_lora. Implementations: the SX1262 on a
 * T-Deck or Heltec (sx1262_phy.cpp), a Heltec used as a USB radio modem
 * from the PC (radio_serial_phy.c), and a fake pair in test_lora.
 *
 * Packets here are already link-encoded (radio_link.h). The PHY does not
 * look inside them, fragment them, or retry them.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cybiko_radio_phy cybiko_radio_phy_t;

struct cybiko_radio_phy {
    void *impl;
    /* Start sending one packet. 0 = accepted, -1 = busy or refused.
     * Must return without waiting for the airtime. */
    int (*tx)(cybiko_radio_phy_t *self, const uint8_t *pkt, size_t len);
    /* 1 while a packet accepted by tx() is still on the air. */
    int (*tx_busy)(cybiko_radio_phy_t *self);
    /* Copy out one received packet. 1 = got one, 0 = none. */
    int (*rx)(cybiko_radio_phy_t *self, uint8_t *buf, size_t cap, size_t *len);
    /* Service the hardware or the serial link. Called from the transport's
     * pump; never from the CPU instruction loop. May be NULL. */
    void (*poll)(cybiko_radio_phy_t *self);
    size_t mtu;  /* largest packet tx() takes, at most CYBIKO_LINK_MAX_PHYS */
};

#ifdef __cplusplus
}
#endif

#endif
