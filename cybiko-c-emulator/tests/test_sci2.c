#include "acutest.h"
#include "core/address_bus.h"
#include "core/h8s_cpu.h"
#include "radio/radio_coproc.h"
#include "radio/radio_sim.h"
#include "radio/sci2_xt.h"

#include <string.h>

#define ON_CHIP_BASE 0xFFDC00u
#define ID_A 0x10000001u
#define ID_B 0x10000002u

typedef struct {
    address_bus_t bus;
    h8s_cpu_t cpu;
    cybiko_radio_coproc_t coproc;
    radio_sim_node_t node;
} node_t;

static node_t node_a;
static node_t node_b;
static radio_sim_bus_t sim;

static void open_node(node_t *n, uint32_t id, bool external_ram) {
    memset(n, 0, sizeof *n);
    bus_init(&n->bus);
    memory_init(&n->bus.on_chip_ram, 0x2400, true);
    if (external_ram)
        memory_init(&n->bus.external_ram, 0x200000, true);
    h8s_cpu_init(&n->cpu, &n->bus);
    n->bus.cpu = &n->cpu;
    cybiko_radio_coproc_init(&n->coproc);
    radio_sim_node_init(&n->node, &sim, id, 255);
    radio_sim_bus_add(&sim, &n->node);
    cybiko_radio_coproc_set_transport(&n->coproc, &n->node.base);
    bus_attach_radio(&n->bus, &n->coproc);
}

static void close_node(node_t *n) {
    bus_free(&n->bus);
}

static void begin_pair(void) {
    close_node(&node_a);
    close_node(&node_b);
    radio_sim_bus_init(&sim);
    open_node(&node_a, ID_A, true);
    open_node(&node_b, ID_B, true);
}

static void end_pair(void) {
    close_node(&node_a);
    close_node(&node_b);
}

static bool has_irq(const h8s_cpu_t *cpu, int vector) {
    for (int i = 0; i < cpu->pending_irq_count; i++) {
        if (cpu->pending_irqs[i] == vector)
            return true;
    }
    return false;
}

static void program_dtc(address_bus_t *bus, uint32_t base, uint8_t mra,
                        uint32_t sar, uint32_t dar, uint16_t count) {
    bus_write8(bus, base + 0, mra);
    bus_write8(bus, base + 1, (uint8_t)(sar >> 16));
    bus_write8(bus, base + 2, (uint8_t)(sar >> 8));
    bus_write8(bus, base + 3, (uint8_t)sar);
    bus_write8(bus, base + 4, (uint8_t)(dar >> 24));
    bus_write8(bus, base + 5, (uint8_t)(dar >> 16));
    bus_write8(bus, base + 6, (uint8_t)(dar >> 8));
    bus_write8(bus, base + 7, (uint8_t)dar);
    bus_write8(bus, base + 8, (uint8_t)(count >> 8));
    bus_write8(bus, base + 9, (uint8_t)count);
}

static void plant_xt_beacon(address_bus_t *bus, uint32_t addr, uint32_t cyid, const char *name) {
    uint8_t raw[51];
    memset(raw, 0, sizeof raw);
    memset(raw, 0xFF, 4);
    raw[4] = (uint8_t)(cyid >> 24);
    raw[5] = (uint8_t)(cyid >> 16);
    raw[6] = (uint8_t)(cyid >> 8);
    raw[7] = (uint8_t)cyid;
    raw[8] = 0xC2;
    raw[9] = 0x01;
    memcpy(raw + 20, name, strlen(name));
    for (size_t i = 0; i < sizeof raw; i++)
        bus_write8(bus, addr + (uint32_t)i, raw[i]);
}

static void trigger_tx(address_bus_t *bus) {
    bus_write8(bus, CYBIKO_SCI2_DTCERF, 0x40);
    bus_write8(bus, CYBIKO_SCI2_SCR, 0xA0);
}

static uint8_t pull_rdr(address_bus_t *bus) {
    bus_tick_sci2(bus, 1);
    TEST_CHECK((bus_read8(bus, CYBIKO_SCI2_SSR) & CYBIKO_SCI2_SSR_RDRF) != 0);
    return bus_read8(bus, CYBIKO_SCI2_RDR);
}

static void test_unattached_sci2_stays_ready(void) {
    address_bus_t bus;
    bus_init(&bus);
    memory_init(&bus.on_chip_ram, 0x2400, true);

    TEST_CHECK(bus_read8(&bus, CYBIKO_SCI2_SSR) ==
               (CYBIKO_SCI2_SSR_TDRE | CYBIKO_SCI2_SSR_TEND));
    bus_write8(&bus, CYBIKO_SCI2_TDR, 0x41);
    bus_tick_sci2(&bus, 64);
    TEST_CHECK(bus_read8(&bus, CYBIKO_SCI2_SSR) ==
               (CYBIKO_SCI2_SSR_TDRE | CYBIKO_SCI2_SSR_TEND));
    bus_write8(&bus, CYBIKO_SCI2_SCR, 0xA0);
    TEST_CHECK(bus_read8(&bus, CYBIKO_SCI2_SCR) == 0xA0);
    bus_write8(&bus, CYBIKO_SCI2_DTCERF, 0x40);
    TEST_CHECK(bus_read8(&bus, CYBIKO_SCI2_DTCERF) == 0x40);

    memory_free(&bus.on_chip_ram);
}

static void test_scr_cache_and_tdre_delay(void) {
    begin_pair();
    address_bus_t *bus = &node_a.bus;

    TEST_CHECK(bus_read8(bus, CYBIKO_SCI2_SSR) ==
               (CYBIKO_SCI2_SSR_TDRE | CYBIKO_SCI2_SSR_TEND));
    bus_write8(bus, CYBIKO_SCI2_SCR, 0xA0);
    memory_write8(&bus->on_chip_ram, CYBIKO_SCI2_SCR - ON_CHIP_BASE, 0x00);
    TEST_CHECK(bus_read8(bus, CYBIKO_SCI2_SCR) == 0xA0);

    node_a.cpu.pending_irq_count = 0;
    bus_write8(bus, CYBIKO_SCI2_TDR, 0x55);
    TEST_CHECK((bus_read8(bus, CYBIKO_SCI2_SSR) & CYBIKO_SCI2_SSR_TDRE) == 0);
    TEST_CHECK((bus_read8(bus, CYBIKO_SCI2_SSR) & CYBIKO_SCI2_SSR_TEND) != 0);
    bus_tick_sci2(bus, CYBIKO_SCI2_TXI_DELAY - 1);
    TEST_CHECK((bus_read8(bus, CYBIKO_SCI2_SSR) & CYBIKO_SCI2_SSR_TDRE) == 0);
    bus_tick_sci2(bus, 1);
    TEST_CHECK((bus_read8(bus, CYBIKO_SCI2_SSR) & CYBIKO_SCI2_SSR_TDRE) != 0);
    TEST_CHECK(has_irq(&node_a.cpu, CYBIKO_SCI2_TXI_VECTOR));
    end_pair();
}

static void test_beacon_crosses_the_bus(void) {
    begin_pair();
    plant_xt_beacon(&node_a.bus, 0x400100, ID_A, "deck");
    program_dtc(&node_a.bus, CYBIKO_SCI2_TX_DTC, 0x80, 0x400100, 0, 51);
    trigger_tx(&node_a.bus);

    TEST_CHECK(has_irq(&node_a.cpu, CYBIKO_SCI2_TXI_VECTOR));
    TEST_CHECK(cybiko_radio_coproc_rx_count(&node_a.coproc) == 0);
    TEST_CHECK(bus_read8(&node_a.bus, CYBIKO_SCI2_DTCERF) == 0x00);
    TEST_CHECK(bus_read8(&node_a.bus, CYBIKO_SCI2_TX_DTC + 8) == 0);
    TEST_CHECK(bus_read8(&node_a.bus, CYBIKO_SCI2_TX_DTC + 9) == 0);
    TEST_CHECK(node_a.coproc.stat_tx == 1);
    TEST_CHECK(!has_irq(&node_a.cpu, CYBIKO_SCI2_RXI_VECTOR));

    radio_sim_bus_pump(&sim);
    TEST_CHECK(cybiko_radio_coproc_pending(&node_b.coproc) == 1);
    TEST_CHECK(cybiko_radio_coproc_pending(&node_a.coproc) == 0);

    bus_write8(&node_a.bus, CYBIKO_SCI2_DTCERF, 0x00);
    TEST_CHECK(pull_rdr(&node_a.bus) == CYBIKO_RADIO_ACK);
    TEST_CHECK(pull_rdr(&node_a.bus) == CYBIKO_RADIO_IND_SMALL);

    plant_xt_beacon(&node_b.bus, 0x400100, ID_B, "peer");
    program_dtc(&node_b.bus, CYBIKO_SCI2_TX_DTC, 0x80, 0x400100, 0, 51);
    trigger_tx(&node_b.bus);
    TEST_CHECK(cybiko_radio_coproc_pending(&node_b.coproc) == 1);
    bus_write8(&node_b.bus, CYBIKO_SCI2_DTCERF, 0x00);
    TEST_CHECK(pull_rdr(&node_b.bus) == CYBIKO_RADIO_ACK);
    TEST_CHECK(pull_rdr(&node_b.bus) == CYBIKO_RADIO_IND_SMALL);

    program_dtc(&node_b.bus, CYBIKO_SCI2_RX_DTC, 0x20, 0, 0x401000, 50);
    bus_write8(&node_b.bus, CYBIKO_SCI2_DTCERF, 0x80);
    TEST_CHECK(bus_read8(&node_b.bus, 0x401000) == 0x00);
    bus_write8(&node_b.bus, CYBIKO_SCI2_SCR, 0x50);

    uint8_t got[50];
    for (int i = 0; i < 50; i++)
        got[i] = bus_read8(&node_b.bus, 0x401000 + (uint32_t)i);
    TEST_CHECK(got[0] == 0xFF && got[1] == 0xFF && got[2] == 0xFF && got[3] == 0xFF);
    TEST_CHECK(got[4] == 0x10 && got[5] == 0x00 && got[6] == 0x00 && got[7] == 0x01);
    TEST_CHECK(got[8] == 0xC2);
    TEST_CHECK(memcmp(got + 20, "deck", 4) == 0);
    TEST_CHECK(bus_read8(&node_b.bus, 0x401000 + 50) == 0x00);
    TEST_CHECK(bus_read8(&node_b.bus, CYBIKO_SCI2_DTCERF) == 0x00);
    TEST_CHECK(bus_read8(&node_b.bus, CYBIKO_SCI2_RX_DTC + 8) == 0);
    TEST_CHECK(pull_rdr(&node_b.bus) == 0xFF);
    TEST_CHECK(has_irq(&node_b.cpu, CYBIKO_SCI2_RXI_VECTOR));

    radio_sim_bus_pump(&sim);
    TEST_CHECK(cybiko_radio_coproc_pending(&node_a.coproc) == 1);
    end_pair();
}

static void test_short_dtc_keeps_a_queued_frame(void) {
    begin_pair();
    plant_xt_beacon(&node_b.bus, 0x400100, ID_B, "peer");
    program_dtc(&node_b.bus, CYBIKO_SCI2_TX_DTC, 0x80, 0x400100, 0, 51);
    trigger_tx(&node_b.bus);
    radio_sim_bus_pump(&sim);
    TEST_CHECK(cybiko_radio_coproc_pending(&node_a.coproc) == 1);

    uint8_t cmd[4] = {0x01, 0x06, 0x19, 0x00};
    for (int i = 0; i < 4; i++)
        bus_write8(&node_a.bus, 0x400200 + (uint32_t)i, cmd[i]);
    program_dtc(&node_a.bus, CYBIKO_SCI2_TX_DTC, 0x80, 0x400200, 0, 4);
    node_a.cpu.pending_irq_count = 0;
    trigger_tx(&node_a.bus);

    TEST_CHECK(has_irq(&node_a.cpu, CYBIKO_SCI2_TXI_VECTOR));
    TEST_CHECK(node_a.coproc.stat_tx == 0);
    TEST_CHECK(cybiko_radio_coproc_pending(&node_a.coproc) == 1);
    TEST_CHECK(cybiko_radio_coproc_rx_count(&node_a.coproc) == 0);
    radio_sim_bus_pump(&sim);
    TEST_CHECK(cybiko_radio_coproc_pending(&node_b.coproc) == 0);

    bus_write8(&node_a.bus, CYBIKO_SCI2_DTCERF, 0x00);
    TEST_CHECK(pull_rdr(&node_a.bus) == CYBIKO_RADIO_ACK);
    TEST_CHECK(pull_rdr(&node_a.bus) == CYBIKO_RADIO_IND_SMALL);
    TEST_CHECK(cybiko_radio_coproc_pending(&node_a.coproc) == 1);
    end_pair();
}

static void test_async_idle_and_fixed_destination(void) {
    begin_pair();
    plant_xt_beacon(&node_a.bus, 0x400100, ID_A, "deck");
    program_dtc(&node_a.bus, CYBIKO_SCI2_TX_DTC, 0x80, 0x400100, 0, 51);
    trigger_tx(&node_a.bus);
    radio_sim_bus_pump(&sim);
    TEST_CHECK(cybiko_radio_coproc_pending(&node_b.coproc) == 1);

    bus_write32(&node_b.bus, 0x4B49FE, 0x4C0000);
    bus_write8(&node_b.bus, 0x4C0000 + 0x335A, 0);
    bus_tick_sci2(&node_b.bus, 1);
    TEST_CHECK(cybiko_radio_coproc_rx_count(&node_b.coproc) == 0);

    bus_write8(&node_b.bus, 0x4C0000 + 0x335A, 1);
    bus_tick_sci2(&node_b.bus, CYBIKO_SCI2_ASYNC_INTERVAL);
    TEST_CHECK(cybiko_radio_coproc_rx_count(&node_b.coproc) == 0);
    bus_tick_sci2(&node_b.bus, 1);
    TEST_CHECK(cybiko_radio_coproc_rx_count(&node_b.coproc) == 1);
    TEST_CHECK((bus_read8(&node_b.bus, CYBIKO_SCI2_SSR) & CYBIKO_SCI2_SSR_RDRF) == 0);
    bus_tick_sci2(&node_b.bus, 1);
    TEST_CHECK(bus_read8(&node_b.bus, CYBIKO_SCI2_RDR) == CYBIKO_RADIO_IND_SMALL);

    program_dtc(&node_b.bus, CYBIKO_SCI2_RX_DTC, 0x00, 0, 0x402000, 4);
    bus_write8(&node_b.bus, CYBIKO_SCI2_DTCERF, 0x80);
    bus_write8(&node_b.bus, CYBIKO_SCI2_SCR, 0x50);
    TEST_CHECK(bus_read8(&node_b.bus, 0x402000) == 0xFF);
    TEST_CHECK(bus_read8(&node_b.bus, 0x402001) == 0x00);
    TEST_CHECK(cybiko_radio_coproc_pending(&node_b.coproc) == 0);
    TEST_CHECK(pull_rdr(&node_b.bus) == 0xFF);
    TEST_CHECK(has_irq(&node_b.cpu, CYBIKO_SCI2_RXI_VECTOR));
    end_pair();
}

TEST_LIST = {
    { "unattached SCI2 stays transmit-ready", test_unattached_sci2_stays_ready },
    { "SCR cache and TDRE delay", test_scr_cache_and_tdre_delay },
    { "beacon TX buffer reaches peer RX buffer", test_beacon_crosses_the_bus },
    { "short DTC does not drop a queued frame", test_short_dtc_keeps_a_queued_frame },
    { "idle injection and fixed RX destination", test_async_idle_and_fixed_destination },
    { NULL, NULL }
};
