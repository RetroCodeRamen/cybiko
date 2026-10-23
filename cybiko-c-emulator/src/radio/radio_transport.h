#ifndef CYBIKO_RADIO_TRANSPORT_H
#define CYBIKO_RADIO_TRANSPORT_H

/*
 * Transport under the emulated AVR radio co-processor.
 *
 * The payload is the guest frame after the RF2915 preamble/CyID header has
 * been removed (42-byte beacon or 192-byte chat on current CyOS). The
 * co-processor does not know whether the bytes move by loopback, UDP, or
 * LoRa. Behavior matches RadioTransport in the Java reference emulator
 * (MIT, Copyright 2026 Dan Berkowitz).
 *
 * send_packet must not block the CPU emulator. The host calls pump() from
 * a radio task or the test harness.
 *
 * Channel is recorded, not filtered. CyOS hops between channels 2 and 4
 * while a packet may still be in flight.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Starting SX1262 profile for a region-configurable radio.
 * 915 MHz is a US915 default, not a permit to transmit. The operator
 * selects the frequency, and the driver must be able to refuse a send
 * that would break a local duty cycle. These are not yet programmed
 * into a chip. */
#define CYBIKO_LORA_FREQ_HZ       915000000u
/* 500 kHz: a 192-byte Chat frame is ~85 ms on the air instead of ~330 ms,
 * so CyOS's three copies of a message leave the channel free sooner. In
 * the US, a fixed channel this wide is digital modulation under FCC
 * 15.247 rather than the ~0 dBm 15.249 limit a 125 kHz channel has. */
#define CYBIKO_LORA_BW_HZ         500000u
#define CYBIKO_LORA_SF            7
#define CYBIKO_LORA_CR_DENOM      5       /* 4/5 */
#ifndef CYBIKO_LORA_POWER_DBM     /* bench builds pass a lower value */
#define CYBIKO_LORA_POWER_DBM     14
#endif
#define CYBIKO_LORA_SYNC_WORD     0x1424  /* private LoRa, not LoRaWAN public */
#define CYBIKO_LORA_PREAMBLE      8
#define CYBIKO_LORA_PHY_MAX       255

typedef struct cybiko_radio_transport cybiko_radio_transport_t;

typedef void (*cybiko_radio_on_packet_fn)(
    void *user,
    const uint8_t *data,
    size_t len,
    uint8_t channel,
    uint32_t sender_id);

struct cybiko_radio_transport {
    uint32_t device_id;
    void *impl;
    int (*start)(cybiko_radio_transport_t *self);
    void (*close)(cybiko_radio_transport_t *self);
    /* 0 = queued, -1 = rejected. Never waits on the PHY. */
    int (*send_packet)(cybiko_radio_transport_t *self,
                       const uint8_t *data, size_t len, uint8_t channel);
    void (*set_channel)(cybiko_radio_transport_t *self, uint8_t channel);
    void (*set_listener)(cybiko_radio_transport_t *self,
                         cybiko_radio_on_packet_fn fn, void *user);
    void (*pump)(cybiko_radio_transport_t *self);
};

#ifdef __cplusplus
}
#endif

#endif
