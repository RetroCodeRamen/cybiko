#include "acutest.h"
#include "core/speaker.h"
#include <string.h>

#define CLOCK_HZ 18432000

static void test_init(void) {
    speaker_t spk;
    speaker_init(&spk, CLOCK_HZ);
    TEST_CHECK(spk.current_level == 0);
    TEST_CHECK(spk.frame_start_level == 0);
    TEST_CHECK(spk.transition_count == 0);
    TEST_CHECK(spk.cycles_per_sample == (double)CLOCK_HZ / SPEAKER_SAMPLE_RATE);
}

static void test_no_transitions_silence(void) {
    speaker_t spk;
    speaker_init(&spk, CLOCK_HZ);
    speaker_begin_frame(&spk);
    /* No set_level calls */
    uint8_t out[1024];
    int n = speaker_generate_samples(&spk, 307200, out, 1024);
    TEST_CHECK(n > 0);
    for (int i = 0; i < n; i++) {
        TEST_CHECK_(out[i] == 128, "sample %d: expected 128, got %d", i, out[i]);
        if (out[i] != 128) break;
    }
}

static void test_single_transition_midway(void) {
    speaker_t spk;
    speaker_init(&spk, CLOCK_HZ);
    speaker_begin_frame(&spk);

    /* Transition at cycle 153600 (midpoint of frame) */
    spk.frame_cycle = 153600;
    speaker_set_level(&spk, 1);

    uint8_t out[1024];
    int n = speaker_generate_samples(&spk, 307200, out, 1024);
    TEST_CHECK(n > 0);

    /* First sample should be LOW (96), last should be HIGH (160) */
    TEST_CHECK_(out[0] == 96, "first sample: expected 96, got %d", out[0]);
    TEST_CHECK_(out[n - 1] == 160, "last sample: expected 160, got %d", out[n - 1]);
}

static void test_set_level_deduplicates(void) {
    speaker_t spk;
    speaker_init(&spk, CLOCK_HZ);
    speaker_begin_frame(&spk);

    spk.frame_cycle = 100;
    speaker_set_level(&spk, 1);
    speaker_set_level(&spk, 1); /* Same level, should be ignored */
    TEST_CHECK(spk.transition_count == 1);
}

static void test_begin_frame_resets(void) {
    speaker_t spk;
    speaker_init(&spk, CLOCK_HZ);
    speaker_begin_frame(&spk);

    spk.frame_cycle = 100;
    speaker_set_level(&spk, 1);
    TEST_CHECK(spk.transition_count == 1);

    speaker_begin_frame(&spk);
    TEST_CHECK(spk.transition_count == 0);
}

static void test_frame_start_level_preserved(void) {
    speaker_t spk;
    speaker_init(&spk, CLOCK_HZ);

    /* First frame: transition to HIGH */
    speaker_begin_frame(&spk);
    spk.frame_cycle = 100;
    speaker_set_level(&spk, 1);
    TEST_CHECK(spk.current_level == 1);

    /* Begin next frame: frame_start_level should be 1 */
    speaker_begin_frame(&spk);
    TEST_CHECK(spk.frame_start_level == 1);
}

static void test_sample_count(void) {
    speaker_t spk;
    speaker_init(&spk, CLOCK_HZ);
    speaker_begin_frame(&spk);

    uint8_t out[2048];
    /* 307200 cycles per frame at 18.432MHz, 48kHz sample rate
       = 307200 / 384 = 800 samples per frame */
    int n = speaker_generate_samples(&spk, 307200, out, 2048);
    TEST_CHECK_(n == 800, "expected 800 samples, got %d", n);
}

static void test_transition_to_low(void) {
    speaker_t spk;
    speaker_init(&spk, CLOCK_HZ);

    /* Start HIGH */
    speaker_set_level(&spk, 1);
    speaker_begin_frame(&spk);
    TEST_CHECK(spk.frame_start_level == 1);

    /* Transition to LOW at beginning */
    spk.frame_cycle = 0;
    speaker_set_level(&spk, 0);

    uint8_t out[1024];
    int n = speaker_generate_samples(&spk, 307200, out, 1024);
    TEST_CHECK(n > 0);
    /* All samples should be LOW (96) */
    TEST_CHECK_(out[0] == 96, "first sample: expected 96, got %d", out[0]);
    TEST_CHECK_(out[n - 1] == 96, "last sample: expected 96, got %d", out[n - 1]);
}

TEST_LIST = {
    { "init",                       test_init },
    { "no_transitions_silence",     test_no_transitions_silence },
    { "single_transition_midway",   test_single_transition_midway },
    { "set_level_deduplicates",     test_set_level_deduplicates },
    { "begin_frame_resets",         test_begin_frame_resets },
    { "frame_start_level_preserved", test_frame_start_level_preserved },
    { "sample_count",               test_sample_count },
    { "transition_to_low",          test_transition_to_low },
    { NULL, NULL }
};
