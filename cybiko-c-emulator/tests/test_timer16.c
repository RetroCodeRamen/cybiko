#include "acutest.h"
#include "core/timer16.h"
#include "core/h8s_cpu.h"
#include <string.h>

static h8s_cpu_t cpu;
static int cb_level = -1;
static int cb_count = 0;

static void cpu_init_stub(void) {
    memset(&cpu, 0, sizeof(cpu));
}

static void output_b_callback(void *ctx, int level) {
    (void)ctx;
    cb_level = level;
    cb_count++;
}

static void test_init_defaults(void) {
    timer16_t t;
    cpu_init_stub();
    timer16_init(&t, 1, 2, 40, &cpu);
    TEST_CHECK(t.tcnt == 0);
    TEST_CHECK(t.tgra == 0xFFFF);
    TEST_CHECK(t.tgrb == 0xFFFF);
    TEST_CHECK(t.tcr == 0);
    TEST_CHECK(t.tior == 0);
    TEST_CHECK(t.tier == 0);
    TEST_CHECK(t.tsr == 0);
    TEST_CHECK(t.enabled == false);
    TEST_CHECK(t.output_a_level == 0);
    TEST_CHECK(t.output_b_level == 0);
}

static void test_init_vectors(void) {
    timer16_t t;
    cpu_init_stub();
    timer16_init(&t, 1, 2, 40, &cpu);
    TEST_CHECK(t.vec_tgia == 40);
    TEST_CHECK(t.vec_tgib == 41);
    TEST_CHECK(t.vec_ovf == 42); /* base + tgr_count */
}

static void test_not_running_when_disabled(void) {
    timer16_t t;
    cpu_init_stub();
    timer16_init(&t, 1, 2, 40, &cpu);
    /* Set clock but leave disabled */
    timer16_write8(&t, 0, 0x01); /* CKS=1 (div1) */
    TEST_CHECK(!timer16_is_running(&t));
    timer16_tick(&t);
    TEST_CHECK(t.tcnt == 0);
}

static void test_prescaler_div1(void) {
    timer16_t t;
    cpu_init_stub();
    timer16_init(&t, 0, 4, 32, &cpu); /* Channel 0 */
    timer16_write8(&t, 0, 0x00); /* CKS=0 → div1 for ch0 */
    timer16_set_enabled(&t, true);
    timer16_tick(&t);
    TEST_CHECK_(t.tcnt == 1, "expected 1, got %u", t.tcnt);
}

static void test_prescaler_div16(void) {
    timer16_t t;
    cpu_init_stub();
    timer16_init(&t, 0, 4, 32, &cpu);
    timer16_write8(&t, 0, 0x02); /* CKS=2 → div16 for ch0 */
    timer16_set_enabled(&t, true);
    for (int i = 0; i < 15; i++) timer16_tick(&t);
    TEST_CHECK(t.tcnt == 0);
    timer16_tick(&t);
    TEST_CHECK(t.tcnt == 1);
}

static void test_compare_match_a_sets_flag(void) {
    timer16_t t;
    cpu_init_stub();
    timer16_init(&t, 0, 4, 32, &cpu);
    timer16_write8(&t, 0, 0x00); /* div1 */
    timer16_write16(&t, 8, 5);   /* TGRA=5 */
    timer16_set_enabled(&t, true);
    for (int i = 0; i < 5; i++) timer16_tick(&t);
    TEST_CHECK((t.tsr & 0x01) != 0); /* TGFA set */
}

static void test_compare_match_b_sets_flag(void) {
    timer16_t t;
    cpu_init_stub();
    timer16_init(&t, 0, 4, 32, &cpu);
    timer16_write8(&t, 0, 0x00);
    timer16_write16(&t, 0xA, 3); /* TGRB=3 */
    timer16_set_enabled(&t, true);
    for (int i = 0; i < 3; i++) timer16_tick(&t);
    TEST_CHECK((t.tsr & 0x02) != 0); /* TGFB set */
}

static void test_clear_on_tgra(void) {
    timer16_t t;
    cpu_init_stub();
    timer16_init(&t, 0, 4, 32, &cpu);
    /* TCR: clear_mode=1 (bits 6:5 = 01), CKS=0 (div1) → 0x20 */
    timer16_write8(&t, 0, 0x20);
    timer16_write16(&t, 8, 4); /* TGRA=4 */
    timer16_set_enabled(&t, true);
    for (int i = 0; i < 4; i++) timer16_tick(&t);
    TEST_CHECK_(t.tcnt == 0, "expected 0 after clear, got %u", t.tcnt);
}

static void test_overflow(void) {
    timer16_t t;
    cpu_init_stub();
    timer16_init(&t, 0, 4, 32, &cpu);
    timer16_write8(&t, 0, 0x00); /* div1 */
    timer16_set_enabled(&t, true);
    /* Set counter near overflow */
    timer16_write16(&t, 6, 0xFFFE);
    timer16_tick(&t); /* 0xFFFE → 0xFFFF */
    TEST_CHECK(!(t.tsr & 0x10)); /* OVF not set yet */
    timer16_tick(&t); /* 0xFFFF → 0x0000 */
    TEST_CHECK((t.tsr & 0x10) != 0); /* OVF set */
}

static void test_tior_initial_level(void) {
    timer16_t t;
    cpu_init_stub();
    timer16_init(&t, 1, 2, 40, &cpu);
    cb_level = -1;
    cb_count = 0;
    t.output_b_cb = output_b_callback;
    /* TIOR=0x50: IOB=5 (0101) → initial HIGH, clear on match */
    timer16_write8(&t, 2, 0x50);
    TEST_CHECK_(t.output_b_level == 1, "expected HIGH (1), got %d", t.output_b_level);
    TEST_CHECK(cb_level == 1);
}

static void test_tior_match_actions(void) {
    timer16_t t;
    cpu_init_stub();

    /* Test clear (01): initial HIGH, go LOW on match */
    timer16_init(&t, 1, 2, 40, &cpu);
    cb_level = -1;
    cb_count = 0;
    t.output_b_cb = output_b_callback;
    timer16_write8(&t, 2, 0x50); /* IOB=5: initial HIGH, clear on match */
    timer16_write8(&t, 0, 0x00); /* div1 */
    timer16_write16(&t, 0xA, 2); /* TGRB=2 */
    timer16_set_enabled(&t, true);
    for (int i = 0; i < 2; i++) timer16_tick(&t);
    TEST_CHECK_(t.output_b_level == 0, "expected LOW after match, got %d", t.output_b_level);

    /* Test set (10): initial LOW, go HIGH on match */
    timer16_init(&t, 1, 2, 40, &cpu);
    cb_level = -1;
    cb_count = 0;
    t.output_b_cb = output_b_callback;
    timer16_write8(&t, 2, 0x20); /* IOB=2: initial LOW, set on match */
    timer16_write8(&t, 0, 0x00);
    timer16_write16(&t, 0xA, 2);
    timer16_set_enabled(&t, true);
    for (int i = 0; i < 2; i++) timer16_tick(&t);
    TEST_CHECK_(t.output_b_level == 1, "expected HIGH after set, got %d", t.output_b_level);

    /* Test toggle (11): initial LOW, toggle on match */
    timer16_init(&t, 1, 2, 40, &cpu);
    cb_level = -1;
    cb_count = 0;
    t.output_b_cb = output_b_callback;
    timer16_write8(&t, 2, 0x30); /* IOB=3: initial LOW, toggle on match */
    timer16_write8(&t, 0, 0x00);
    timer16_write16(&t, 0xA, 2);
    timer16_set_enabled(&t, true);
    for (int i = 0; i < 2; i++) timer16_tick(&t);
    TEST_CHECK_(t.output_b_level == 1, "expected HIGH after toggle, got %d", t.output_b_level);
}

static void test_pwm_waveform(void) {
    timer16_t t;
    cpu_init_stub();
    timer16_init(&t, 1, 2, 40, &cpu);
    cb_level = -1;
    cb_count = 0;
    t.output_b_cb = output_b_callback;

    /* Configure PWM: TIOR=0x50 (IOB=5: initial HIGH, clear on match) */
    timer16_write8(&t, 2, 0x50);
    /* TCR: clear_mode=1 (clear on TGRA), CKS=0 (div1) → 0x20 */
    timer16_write8(&t, 0, 0x20);
    timer16_write16(&t, 8, 10);  /* TGRA=10 (period) */
    timer16_write16(&t, 0xA, 5); /* TGRB=5 (50% duty) */
    timer16_set_enabled(&t, true);

    /* Count=0: output should be HIGH (initial) */
    TEST_CHECK(t.output_b_level == 1);

    /* Tick to TGRB match (count reaches 5) */
    for (int i = 0; i < 5; i++) timer16_tick(&t);
    TEST_CHECK_(t.output_b_level == 0, "expected LOW at TGRB match, got %d", t.output_b_level);

    /* Tick to TGRA match (count reaches 10) */
    for (int i = 0; i < 5; i++) timer16_tick(&t);
    TEST_CHECK_(t.output_b_level == 1, "expected HIGH at TGRA clear, got %d", t.output_b_level);
}

static void test_output_b_callback_fires(void) {
    timer16_t t;
    cpu_init_stub();
    timer16_init(&t, 1, 2, 40, &cpu);
    cb_level = -1;
    cb_count = 0;
    t.output_b_cb = output_b_callback;

    timer16_write8(&t, 2, 0x50); /* IOB=5 → initial HIGH */
    TEST_CHECK(cb_count == 1);
    /* Setting same level again shouldn't fire */
    timer16_write8(&t, 2, 0x50);
    TEST_CHECK(cb_count == 1);
}

static void test_channel_clock_divisors(void) {
    timer16_t t;
    cpu_init_stub();

    /* Channel 1: CKS=6 → div256 */
    timer16_init(&t, 1, 2, 40, &cpu);
    timer16_write8(&t, 0, 0x06);
    timer16_set_enabled(&t, true);
    TEST_CHECK(timer16_is_running(&t));
    for (int i = 0; i < 255; i++) timer16_tick(&t);
    TEST_CHECK(t.tcnt == 0);
    timer16_tick(&t);
    TEST_CHECK(t.tcnt == 1);

    /* Channel 0: CKS=6 → div0 (stopped) */
    timer16_init(&t, 0, 4, 32, &cpu);
    timer16_write8(&t, 0, 0x06);
    timer16_set_enabled(&t, true);
    TEST_CHECK(!timer16_is_running(&t));
}

static void test_register_roundtrips(void) {
    timer16_t t;
    cpu_init_stub();
    timer16_init(&t, 1, 2, 40, &cpu);

    timer16_write16(&t, 6, 0x1234);
    TEST_CHECK(timer16_read16(&t, 6) == 0x1234);

    timer16_write16(&t, 8, 0xABCD);
    TEST_CHECK(timer16_read16(&t, 8) == 0xABCD);

    timer16_write16(&t, 0xA, 0x5678);
    TEST_CHECK(timer16_read16(&t, 0xA) == 0x5678);

    timer16_write8(&t, 4, 0x13);
    TEST_CHECK(timer16_read8(&t, 4) == 0x13);
}

TEST_LIST = {
    { "init_defaults",            test_init_defaults },
    { "init_vectors",             test_init_vectors },
    { "not_running_when_disabled", test_not_running_when_disabled },
    { "prescaler_div1",           test_prescaler_div1 },
    { "prescaler_div16",          test_prescaler_div16 },
    { "compare_match_a_sets_flag", test_compare_match_a_sets_flag },
    { "compare_match_b_sets_flag", test_compare_match_b_sets_flag },
    { "clear_on_tgra",            test_clear_on_tgra },
    { "overflow",                  test_overflow },
    { "tior_initial_level",        test_tior_initial_level },
    { "tior_match_actions",        test_tior_match_actions },
    { "pwm_waveform",             test_pwm_waveform },
    { "output_b_callback_fires",  test_output_b_callback_fires },
    { "channel_clock_divisors",   test_channel_clock_divisors },
    { "register_roundtrips",      test_register_roundtrips },
    { NULL, NULL }
};
