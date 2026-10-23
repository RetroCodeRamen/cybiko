/*
 * Two co-processors, one simulated LoRa PHY.
 * This checks the guest radio boundary. It does not boot CyOS.
 */
#include "radio/radio_coproc.h"
#include "radio/radio_sim.h"

#include <stdio.h>
#include <string.h>

static int fail(const char *msg) {
    fprintf(stderr, "harness: %s\n", msg);
    return 1;
}

int main(void) {
    static radio_sim_bus_t bus;
    static radio_sim_node_t node[2];
    static cybiko_radio_coproc_t cpu[2];

    radio_sim_bus_init(&bus);
    for (int i = 0; i < 2; i++) {
        radio_sim_node_init(&node[i], &bus, 0xA0000001u + (uint32_t)i, 64);
        radio_sim_bus_add(&bus, &node[i]);
        cybiko_radio_coproc_init(&cpu[i]);
        cybiko_radio_coproc_set_transport(&cpu[i], &node[i].base);
    }

    uint8_t beacon[42];
    memset(beacon, 0, sizeof beacon);
    beacon[0] = 0xC2;
    beacon[1] = 0x01;
    memcpy(beacon + 12, "deck", 4);

    uint8_t chat[192];
    memset(chat, 0, sizeof chat);
    chat[0] = 0xC4;
    chat[1] = 0x20;
    const char *msg = "ping";
    chat[18] = 4;
    memcpy(chat + 19, msg, 4);

    uint8_t raw[210];
    memset(raw, 0xFF, 4);
    raw[4] = 0xA0;
    raw[5] = 0x00;
    raw[6] = 0x00;
    raw[7] = 0x01;
    memcpy(raw + 8, beacon, 42);
    cybiko_radio_coproc_handle_transmit(&cpu[0], raw, 51);
    memcpy(raw + 8, chat, 192);
    cybiko_radio_coproc_handle_transmit(&cpu[0], raw, 201);

    radio_sim_bus_pump(&bus);
    if (cybiko_radio_coproc_pending(&cpu[1]) != 2)
        return fail("peer did not receive both frames");
    if (cybiko_radio_coproc_pending(&cpu[0]) != 0)
        return fail("sender heard its own frames");

    cybiko_radio_coproc_receive(&cpu[1], 0x30);
    cybiko_radio_coproc_receive(&cpu[1], 0x00);
    if (cybiko_radio_coproc_complete_tx_dtc(&cpu[1]) != 0x32)
        return fail("poll did not offer the beacon");
    cybiko_radio_coproc_prepare_rx(&cpu[1], 50);
    uint8_t beacon_rx[50];
    for (int i = 0; i < 50; i++)
        beacon_rx[i] = (uint8_t)cybiko_radio_coproc_read(&cpu[1]);
    if (memcmp(beacon_rx + 20, "deck", 4) != 0)
        return fail("beacon username did not survive");

    cybiko_radio_coproc_receive(&cpu[1], 0xCF);
    cybiko_radio_coproc_receive(&cpu[1], 0x00);
    if (cybiko_radio_coproc_complete_tx_dtc(&cpu[1]) != 0xC8)
        return fail("scan did not offer the chat frame");
    cybiko_radio_coproc_prepare_rx(&cpu[1], 200);

    uint8_t rx[200];
    for (int i = 0; i < 200; i++)
        rx[i] = (uint8_t)cybiko_radio_coproc_read(&cpu[1]);
    if (memcmp(rx + 27, "ping", 4) != 0)
        return fail("chat text did not survive fragmentation");
    if (rx[4] != 0xA0 || rx[7] != 0x01)
        return fail("sender CyID was not preserved");

    printf("harness: beacon + chat survived a 64-byte PHY between two co-processors\n");
    printf("harness: this is the radio boundary, not a CyOS boot and not a T-Deck\n");
    return 0;
}
