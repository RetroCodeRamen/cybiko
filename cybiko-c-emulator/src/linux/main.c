/*
 * Cybiko Emulator - SDL2 Linux Client
 *
 * Boots the Cybiko XT emulator with SDL2 display, keyboard, and audio.
 *
 * Usage: ./cybiko-emu <boot.bin> <flash.bin> [--nvram file] [--app file.app ...] [--trace]
 *                     [--radio /dev/ttyUSB0] [--cyid 4C000001]
 *
 * --radio puts the guest radio on a Heltec running cybiko-heltec-modem.
 * The flash CyID is patched to --cyid (default 0x4C000001) so it differs
 * from a T-Deck booting the same flash.bin.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL2/SDL.h>
#include "core/emulator.h"
#include "core/speaker.h"
#include "core/cfs.h"
#include "radio/radio_coproc.h"
#include "radio/radio_lora.h"
#include "radio/radio_serial_phy.h"

/* ---------------------------------------------------------------------------
 * SDL context - holds handles needed by HAL callbacks
 * --------------------------------------------------------------------------- */

#define MAX_HELD_KEYS      16
#define MAX_PENDING_LETTERS 10
#define MIN_HOLD_FRAMES     3
#define FN_FIRST_DELAY      4
#define FN_NEXT_DELAY       3
#define FN_RELEASE_DELAY   10

typedef struct {
    SDL_Renderer    *renderer;
    SDL_Texture     *texture;
    SDL_AudioDeviceID audio_dev;
    uint16_t         key_matrix[CYBIKO_KEYBOARD_COLUMNS];

    /* Minimum hold: keys stay pressed for at least MIN_HOLD_FRAMES */
    int      held_col[MAX_HELD_KEYS];
    uint16_t held_mask[MAX_HELD_KEYS];
    int      held_frames[MAX_HELD_KEYS];
    bool     held_release_pending[MAX_HELD_KEYS];
    int      held_count;

    /* Fn+letter for number keys: Fn pressed first, letter after delay */
    int      pending_col[MAX_PENDING_LETTERS];
    uint16_t pending_mask[MAX_PENDING_LETTERS];
    int      pending_delay[MAX_PENDING_LETTERS];
    bool     pending_auto_release[MAX_PENDING_LETTERS];
    int      pending_count;
    int      fn_held_count;
    bool     fn_held;
    int      fn_release_countdown;
} sdl_ctx_t;

/* ---------------------------------------------------------------------------
 * HAL callbacks
 * --------------------------------------------------------------------------- */

/* render_frame: grayscale pixels (0-255) -> ARGB8888 texture -> present */
static void hal_render_frame(void *ctx, const uint8_t *pixels, int w, int h)
{
    sdl_ctx_t *sdl = ctx;
    uint32_t *tex_pixels;
    int pitch;

    SDL_LockTexture(sdl->texture, NULL, (void **)&tex_pixels, &pitch);
    for (int i = 0; i < w * h; i++) {
        uint8_t g = pixels[i];
        tex_pixels[i] = 0xFF000000u | ((uint32_t)g << 16) | ((uint32_t)g << 8) | g;
    }
    SDL_UnlockTexture(sdl->texture);
    SDL_RenderCopy(sdl->renderer, sdl->texture, NULL, NULL);
    SDL_RenderPresent(sdl->renderer);
}

/* keyboard_poll: copy key matrix state into emulator buffer */
static void hal_keyboard_poll(void *ctx, uint16_t *matrix, int num_cols)
{
    sdl_ctx_t *sdl = ctx;
    int cols = num_cols < CYBIKO_KEYBOARD_COLUMNS ? num_cols : CYBIKO_KEYBOARD_COLUMNS;
    memcpy(matrix, sdl->key_matrix, (size_t)cols * sizeof(uint16_t));
}

/* audio_output: queue raw samples for playback */
static void hal_audio_output(void *ctx, const uint8_t *samples, int count)
{
    sdl_ctx_t *sdl = ctx;
    SDL_QueueAudio(sdl->audio_dev, samples, (Uint32)count);
}

/* serial_output: print debug byte to stderr */
static void hal_serial_output(void *ctx, uint8_t byte)
{
    (void)ctx;
    fputc(byte, stderr);
}

/* ---------------------------------------------------------------------------
 * Cybiko XT keyboard mapping (ported from SwingRenderer.java handleKeyXT)
 *
 * Maps SDL scancodes to the 15-column × 16-bit matrix.
 * PC Ctrl = Cybiko Fn, PC Home = Select, PC Insert = As,
 * PC End = Help, PC F8 = On/Off.
 *
 * Two mechanisms match the Java emulator:
 *  1. Minimum hold (3 frames) - prevents missed keys from fast press/release
 *  2. Fn+letter timing - Fn pressed first, letter after delay, Fn held after
 * --------------------------------------------------------------------------- */

typedef struct {
    SDL_Scancode scancode;
    int          col;
    uint16_t     mask;
} key_mapping_t;

/* Regular key map (not number keys) */
static const key_mapping_t key_map[] = {
    /* Column 0 */
    { SDL_SCANCODE_F7,        0, 0x0001 },
    { SDL_SCANCODE_M,         0, 0x0100 },
    { SDL_SCANCODE_K,         0, 0x0800 },
    { SDL_SCANCODE_I,         0, 0x1000 },
    { SDL_SCANCODE_O,         0, 0x2000 },
    { SDL_SCANCODE_COMMA,     0, 0x0400 },
    { SDL_SCANCODE_L,         0, 0x4000 },
    /* Column 1 */
    { SDL_SCANCODE_F6,        1, 0x0001 },
    { SDL_SCANCODE_G,         1, 0x0002 },
    { SDL_SCANCODE_B,         1, 0x0004 },
    { SDL_SCANCODE_N,         1, 0x0008 },
    { SDL_SCANCODE_H,         1, 0x0010 },
    { SDL_SCANCODE_Y,         1, 0x0020 },
    { SDL_SCANCODE_U,         1, 0x0040 },
    { SDL_SCANCODE_J,         1, 0x0080 },
    /* Column 2 */
    { SDL_SCANCODE_F5,        2, 0x0001 },
    { SDL_SCANCODE_D,         2, 0x0100 },
    { SDL_SCANCODE_C,         2, 0x0200 },
    { SDL_SCANCODE_V,         2, 0x0800 },
    { SDL_SCANCODE_F,         2, 0x1000 },
    { SDL_SCANCODE_R,         2, 0x2000 },
    { SDL_SCANCODE_T,         2, 0x4000 },
    /* Column 3 */
    { SDL_SCANCODE_F4,        3, 0x0001 },
    { SDL_SCANCODE_Q,         3, 0x0002 },
    { SDL_SCANCODE_A,         3, 0x0004 },
    { SDL_SCANCODE_Z,         3, 0x0008 },
    { SDL_SCANCODE_X,         3, 0x0010 },
    { SDL_SCANCODE_S,         3, 0x0020 },
    { SDL_SCANCODE_W,         3, 0x0040 },
    { SDL_SCANCODE_E,         3, 0x0080 },
    /* Column 4 */
    { SDL_SCANCODE_F3,        4, 0x0001 },
    { SDL_SCANCODE_RETURN,    4, 0x0008 },
    { SDL_SCANCODE_HOME,      4, 0x0010 },  /* Select */
    { SDL_SCANCODE_SPACE,     4, 0x0040 },
    /* Column 5 */
    { SDL_SCANCODE_F2,        5, 0x0001 },
    { SDL_SCANCODE_TAB,       5, 0x0080 },
    { SDL_SCANCODE_BACKSPACE, 5, 0x0100 },
    { SDL_SCANCODE_DELETE,    5, 0x0100 },
    { SDL_SCANCODE_INSERT,    5, 0x0200 },  /* As */
    { SDL_SCANCODE_ESCAPE,    5, 0x0400 },
    /* Column 6 - arrows */
    { SDL_SCANCODE_F1,        6, 0x0001 },
    { SDL_SCANCODE_UP,        6, 0x0800 },
    { SDL_SCANCODE_RIGHT,     6, 0x1000 },
    { SDL_SCANCODE_DOWN,      6, 0x2000 },
    { SDL_SCANCODE_LEFT,      6, 0x4000 },
    /* Column 7 - Fn (mapped to Ctrl) */
    { SDL_SCANCODE_LCTRL,     7, 0x8000 },
    { SDL_SCANCODE_RCTRL,     7, 0x8000 },
    /* Column 8 - Shift */
    { SDL_SCANCODE_LSHIFT,    8, 0x8000 },
    { SDL_SCANCODE_RSHIFT,    8, 0x8000 },
    /* Column 9 - P, period, Help, semicolon (per web emulator) */
    { SDL_SCANCODE_END,       9, 0x0001 },  /* Help */
    { SDL_SCANCODE_PERIOD,    9, 0x0002 },
    { SDL_SCANCODE_SEMICOLON, 9, 0x0008 },
    { SDL_SCANCODE_P,         9, 0x0010 },
};

/* Number key -> Fn+letter mapping (col and mask of the letter) */
static const key_mapping_t num_key_map[] = {
    { SDL_SCANCODE_1,   3, 0x0002 },  /* Fn+Q */
    { SDL_SCANCODE_2,   3, 0x0040 },  /* Fn+W */
    { SDL_SCANCODE_3,   3, 0x0080 },  /* Fn+E */
    { SDL_SCANCODE_4,   2, 0x2000 },  /* Fn+R */
    { SDL_SCANCODE_5,   2, 0x4000 },  /* Fn+T */
    { SDL_SCANCODE_6,   1, 0x0020 },  /* Fn+Y */
    { SDL_SCANCODE_7,   1, 0x0040 },  /* Fn+U */
    { SDL_SCANCODE_8,   0, 0x1000 },  /* Fn+I */
    { SDL_SCANCODE_9,   0, 0x2000 },  /* Fn+O */
    { SDL_SCANCODE_0,   9, 0x0010 },  /* Fn+P */
};

static const int KEY_MAP_COUNT = sizeof(key_map) / sizeof(key_map[0]);
static const int NUM_KEY_MAP_COUNT = sizeof(num_key_map) / sizeof(num_key_map[0]);

/* --- Held key management (minimum hold) --- */

static int find_held(sdl_ctx_t *sdl, int col, uint16_t mask)
{
    for (int i = 0; i < sdl->held_count; i++)
        if (sdl->held_col[i] == col && sdl->held_mask[i] == mask) return i;
    return -1;
}

static void remove_held(sdl_ctx_t *sdl, int idx)
{
    sdl->held_count--;
    if (idx < sdl->held_count) {
        sdl->held_col[idx]    = sdl->held_col[sdl->held_count];
        sdl->held_mask[idx]   = sdl->held_mask[sdl->held_count];
        sdl->held_frames[idx] = sdl->held_frames[sdl->held_count];
        sdl->held_release_pending[idx] = sdl->held_release_pending[sdl->held_count];
    }
}

static void press_key_with_hold(sdl_ctx_t *sdl, int col, uint16_t mask, bool pressed)
{
    if (pressed) {
        sdl->key_matrix[col] |= mask;
        int idx = find_held(sdl, col, mask);
        if (idx >= 0) {
            sdl->held_frames[idx] = MIN_HOLD_FRAMES;
            sdl->held_release_pending[idx] = false;
        } else if (sdl->held_count < MAX_HELD_KEYS) {
            int n = sdl->held_count++;
            sdl->held_col[n]    = col;
            sdl->held_mask[n]   = mask;
            sdl->held_frames[n] = MIN_HOLD_FRAMES;
            sdl->held_release_pending[n] = false;
        }
    } else {
        int idx = find_held(sdl, col, mask);
        if (idx >= 0 && sdl->held_frames[idx] > 0) {
            sdl->held_release_pending[idx] = true;
        } else {
            sdl->key_matrix[col] &= ~mask;
            if (idx >= 0) remove_held(sdl, idx);
        }
    }
}

/* --- Fn+letter pending queue (for number keys) --- */

static void queue_pending_letter(sdl_ctx_t *sdl, int col, uint16_t mask, int delay)
{
    for (int i = 0; i < sdl->pending_count; i++) {
        if (sdl->pending_col[i] == col && sdl->pending_mask[i] == mask) {
            sdl->pending_delay[i] = delay;
            sdl->pending_auto_release[i] = false;
            return;
        }
    }
    if (sdl->pending_count < MAX_PENDING_LETTERS) {
        int n = sdl->pending_count++;
        sdl->pending_col[n]   = col;
        sdl->pending_mask[n]  = mask;
        sdl->pending_delay[n] = delay;
        sdl->pending_auto_release[n] = false;
    }
}

static void mark_pending_auto_release(sdl_ctx_t *sdl, int col, uint16_t mask)
{
    for (int i = 0; i < sdl->pending_count; i++) {
        if (sdl->pending_col[i] == col && sdl->pending_mask[i] == mask) {
            sdl->pending_auto_release[i] = true;
            return;
        }
    }
}

static bool is_pending(sdl_ctx_t *sdl, int col, uint16_t mask)
{
    for (int i = 0; i < sdl->pending_count; i++)
        if (sdl->pending_col[i] == col && sdl->pending_mask[i] == mask) return true;
    return false;
}

static void set_fn_letter(sdl_ctx_t *sdl, int col, uint16_t mask, bool pressed)
{
    if (pressed) {
        sdl->fn_held_count++;
        sdl->fn_release_countdown = 0;
        if (!sdl->fn_held) {
            sdl->key_matrix[7] |= 0x8000;  /* Press Fn first */
            sdl->fn_held = true;
            queue_pending_letter(sdl, col, mask, FN_FIRST_DELAY);
        } else {
            queue_pending_letter(sdl, col, mask, FN_NEXT_DELAY);
        }
    } else {
        if (is_pending(sdl, col, mask)) {
            /* Letter hasn't fired yet - let it fire and auto-release */
            mark_pending_auto_release(sdl, col, mask);
        } else {
            /* Letter already fired - clear it normally */
            sdl->key_matrix[col] &= ~mask;
        }
        if (sdl->fn_held_count > 0) sdl->fn_held_count--;
        if (sdl->fn_held_count == 0 && sdl->fn_held) {
            sdl->fn_release_countdown = FN_RELEASE_DELAY;
        }
    }
}

/* --- Per-frame keyboard tick (call before cybiko_run_frame) --- */

static void keyboard_tick_frame(sdl_ctx_t *sdl)
{
    /* Process minimum hold timers */
    for (int i = 0; i < sdl->held_count; i++) {
        if (sdl->held_frames[i] > 0) sdl->held_frames[i]--;
        if (sdl->held_frames[i] <= 0 && sdl->held_release_pending[i]) {
            sdl->key_matrix[sdl->held_col[i]] &= ~sdl->held_mask[i];
            remove_held(sdl, i);
            i--;
        }
    }

    /* Process pending Fn+letter presses */
    for (int i = 0; i < sdl->pending_count; i++) {
        if (--sdl->pending_delay[i] <= 0) {
            int col = sdl->pending_col[i];
            uint16_t mask = sdl->pending_mask[i];
            bool auto_rel = sdl->pending_auto_release[i];

            sdl->key_matrix[col] |= mask;

            /* Remove from pending queue */
            sdl->pending_count--;
            if (i < sdl->pending_count) {
                sdl->pending_col[i]   = sdl->pending_col[sdl->pending_count];
                sdl->pending_mask[i]  = sdl->pending_mask[sdl->pending_count];
                sdl->pending_delay[i] = sdl->pending_delay[sdl->pending_count];
                sdl->pending_auto_release[i] = sdl->pending_auto_release[sdl->pending_count];
            }
            i--;

            /* If key was already released, auto-release via held mechanism */
            if (auto_rel && sdl->held_count < MAX_HELD_KEYS) {
                int n = sdl->held_count++;
                sdl->held_col[n]    = col;
                sdl->held_mask[n]   = mask;
                sdl->held_frames[n] = MIN_HOLD_FRAMES;
                sdl->held_release_pending[n] = true;
            }
        }
    }

    /* Delayed Fn release */
    if (sdl->fn_release_countdown > 0) {
        sdl->fn_release_countdown--;
        if (sdl->fn_release_countdown == 0 && sdl->fn_held) {
            sdl->key_matrix[7] &= ~0x8000;
            sdl->fn_held = false;
        }
    }
}

/* --- Main key event handler --- */

static void update_key_matrix(sdl_ctx_t *sdl, SDL_KeyboardEvent *key)
{
    /* Filter OS-level key repeat - emulated system handles its own repeat */
    if (key->repeat) return;

    bool pressed = (key->type == SDL_KEYDOWN);
    SDL_Scancode sc = key->keysym.scancode;

    /* Check number keys first (need Fn+letter handling) */
    for (int i = 0; i < NUM_KEY_MAP_COUNT; i++) {
        if (num_key_map[i].scancode == sc) {
            set_fn_letter(sdl, num_key_map[i].col, num_key_map[i].mask, pressed);
            return;
        }
    }

    /* Regular keys with minimum hold */
    for (int i = 0; i < KEY_MAP_COUNT; i++) {
        if (key_map[i].scancode == sc) {
            press_key_with_hold(sdl, key_map[i].col, key_map[i].mask, pressed);
            return;
        }
    }
}

/* ---------------------------------------------------------------------------
 * File loading helper
 * --------------------------------------------------------------------------- */

static uint8_t *load_file(const char *path, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Error: cannot open '%s': %s\n", path, strerror(errno));
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    if (len < 0) {
        fprintf(stderr, "Error: cannot determine size of '%s'\n", path);
        fclose(f);
        return NULL;
    }
    fseek(f, 0, SEEK_SET);

    uint8_t *buf = malloc((size_t)len);
    if (!buf) {
        fprintf(stderr, "Error: out of memory loading '%s'\n", path);
        fclose(f);
        return NULL;
    }

    size_t nread = fread(buf, 1, (size_t)len, f);
    fclose(f);

    if (nread != (size_t)len) {
        fprintf(stderr, "Error: short read on '%s' (expected %ld, got %zu)\n",
                path, len, nread);
        free(buf);
        return NULL;
    }

    *out_size = (size_t)len;
    return buf;
}

/* ---------------------------------------------------------------------------
 * Radio over a serial LoRa modem
 * --------------------------------------------------------------------------- */

static void radio_on_text(void *user, char type, const char *text)
{
    (void)user;
    fprintf(stderr, "Radio modem %s: %s\n", type == 'H' ? "hello" : "status", text);
}

static void radio_print_stats(const cybiko_lora_t *lt, const cybiko_serial_phy_t *sp)
{
    fprintf(stderr, "Radio: tx msgs %u pkts %u rejected %u (modem sent %u timeout %u "
            "refused %u lost %u)  rx pkts %u msgs %u bad %u  rssi %d snr %d\n",
            lt->stats.tx_msgs, lt->stats.tx_pkts, lt->stats.tx_rejected,
            sp->stats.tx_sent, sp->stats.tx_timeout, sp->stats.tx_refused,
            sp->stats.tx_lost, lt->stats.rx_pkts, lt->stats.rx_msgs, lt->stats.rx_bad,
            sp->stats.last_rssi_dbm, sp->stats.last_snr_db);
}

/* ---------------------------------------------------------------------------
 * Main
 * --------------------------------------------------------------------------- */

int main(int argc, char *argv[])
{
    /* ---- Parse command-line arguments ---- */
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <boot.bin> <flash.bin> [--nvram file] [--app file.app ...] [--trace]\n"
                "       [--radio /dev/ttyUSB0] [--cyid 4C000001]\n",
                argv[0]);
        return 1;
    }

    const char *boot_path  = argv[1];
    const char *flash_path = argv[2];
    const char *nvram_path = NULL;
    const char *app_paths[64];
    int app_count = 0;
    bool trace = false;
    const char *radio_path = NULL;
    uint32_t cyid = 0x4C000001u;

    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--nvram") == 0 && i + 1 < argc) {
            nvram_path = argv[++i];
        } else if (strcmp(argv[i], "--app") == 0 && i + 1 < argc) {
            if (app_count < 64) {
                app_paths[app_count++] = argv[++i];
            } else {
                fprintf(stderr, "Warning: too many --app arguments (max 64)\n");
                i++;
            }
        } else if (strcmp(argv[i], "--radio") == 0 && i + 1 < argc) {
            radio_path = argv[++i];
        } else if (strcmp(argv[i], "--cyid") == 0 && i + 1 < argc) {
            cyid = (uint32_t)strtoul(argv[++i], NULL, 16);
        } else if (strcmp(argv[i], "--trace") == 0) {
            trace = true;
        } else {
            fprintf(stderr, "Warning: unknown option '%s'\n", argv[i]);
        }
    }

    (void)trace; /* Used by build system via CYBIKO_TRACE define */

    /* ---- Load ROM files ---- */
    size_t boot_size = 0, flash_size = 0, nvram_size = 0;

    uint8_t *boot_data = load_file(boot_path, &boot_size);
    if (!boot_data) return 1;

    uint8_t *flash_data = load_file(flash_path, &flash_size);
    if (!flash_data) { free(boot_data); return 1; }

    uint8_t *nvram_data = NULL;
    if (nvram_path) {
        nvram_data = load_file(nvram_path, &nvram_size);
        /* NVRAM file may not exist yet - that's okay on first run */
        if (!nvram_data) {
            fprintf(stderr, "Note: NVRAM file '%s' not found, starting fresh\n",
                    nvram_path);
        }
    }

    /* ---- Initialize SDL2 ---- */
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "Error: SDL_Init failed: %s\n", SDL_GetError());
        free(boot_data);
        free(flash_data);
        free(nvram_data);
        return 1;
    }

    SDL_Window *window = SDL_CreateWindow(
        "Cybiko Emulator",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        CYBIKO_LCD_WIDTH * 3, CYBIKO_LCD_HEIGHT * 3,
        SDL_WINDOW_SHOWN
    );
    if (!window) {
        fprintf(stderr, "Error: SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        free(boot_data);
        free(flash_data);
        free(nvram_data);
        return 1;
    }

    SDL_Renderer *renderer = SDL_CreateRenderer(
        window, -1, SDL_RENDERER_ACCELERATED
    );
    if (!renderer) {
        fprintf(stderr, "Error: SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        free(boot_data);
        free(flash_data);
        free(nvram_data);
        return 1;
    }

    SDL_Texture *texture = SDL_CreateTexture(
        renderer,
        SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING,
        CYBIKO_LCD_WIDTH, CYBIKO_LCD_HEIGHT
    );
    if (!texture) {
        fprintf(stderr, "Error: SDL_CreateTexture failed: %s\n", SDL_GetError());
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        free(boot_data);
        free(flash_data);
        free(nvram_data);
        return 1;
    }

    /* ---- Open audio device ---- */
    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq     = SPEAKER_SAMPLE_RATE;
    want.format   = AUDIO_U8;
    want.channels = 1;
    want.samples  = 2048;

    SDL_AudioDeviceID audio_dev = SDL_OpenAudioDevice(
        NULL, 0, &want, &have, 0
    );
    if (audio_dev == 0) {
        fprintf(stderr, "Warning: SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        /* Continue without audio */
    }

    /* ---- Set up SDL context and HAL ---- */
    sdl_ctx_t sdl_ctx;
    memset(&sdl_ctx, 0, sizeof(sdl_ctx));
    sdl_ctx.renderer  = renderer;
    sdl_ctx.texture   = texture;
    sdl_ctx.audio_dev = audio_dev;

    cybiko_hal_t hal;
    hal.render_frame  = hal_render_frame;
    hal.audio_output  = hal_audio_output;
    hal.keyboard_poll = hal_keyboard_poll;
    hal.serial_output = hal_serial_output;
    hal.ctx           = &sdl_ctx;

    /* ---- Create emulator and load ROMs ---- */
    cybiko_emu_t *emu = cybiko_create(&hal);
    if (!emu) {
        fprintf(stderr, "Error: cybiko_create() failed\n");
        if (audio_dev) SDL_CloseAudioDevice(audio_dev);
        SDL_DestroyTexture(texture);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        free(boot_data);
        free(flash_data);
        free(nvram_data);
        return 1;
    }

    if (!cybiko_load_boot_rom(emu, boot_data, boot_size)) {
        fprintf(stderr, "Error: failed to load boot ROM '%s'\n", boot_path);
        cybiko_destroy(emu);
        if (audio_dev) SDL_CloseAudioDevice(audio_dev);
        SDL_DestroyTexture(texture);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        free(boot_data);
        free(flash_data);
        free(nvram_data);
        return 1;
    }

    if (!cybiko_load_flash_rom(emu, flash_data, flash_size)) {
        fprintf(stderr, "Error: failed to load flash ROM '%s'\n", flash_path);
        cybiko_destroy(emu);
        if (audio_dev) SDL_CloseAudioDevice(audio_dev);
        SDL_DestroyTexture(texture);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        free(boot_data);
        free(flash_data);
        free(nvram_data);
        return 1;
    }

    /* ---- Radio ---- */
    static cybiko_serial_phy_t radio_phy;
    static cybiko_lora_t radio_link;
    static cybiko_radio_coproc_t radio_cop;
    bool radio_on = false;
    if (radio_path) {
        if (cybiko_serial_phy_open(&radio_phy, radio_path) != 0) {
            fprintf(stderr, "Warning: radio '%s': %s%s\n",
                    radio_path, strerror(errno),
                    errno == EBUSY ? " — another emulator already has ReBiko Radio"
                                   : ", running without radio");
        } else if (!cybiko_patch_cyid(emu, cyid)) {
            fprintf(stderr, "Warning: CyID patch failed, running without radio\n");
            cybiko_serial_phy_close(&radio_phy);
        } else {
            radio_phy.on_text = radio_on_text;
            cybiko_lora_init(&radio_link, &radio_phy.phy, cyid, true);
            cybiko_lora_seed_msg_id(&radio_link, (uint32_t)SDL_GetPerformanceCounter());
            cybiko_radio_coproc_init(&radio_cop);
            cybiko_radio_coproc_set_transport(&radio_cop, &radio_link.base);
            cybiko_attach_radio(emu, &radio_cop);
            cybiko_serial_phy_request_status(&radio_phy);
            radio_on = true;
            fprintf(stderr, "Radio on %s, CyID %08X\n", radio_path, (unsigned)cyid);
        }
    }

    /* ---- Load NVRAM and inject app files into CFS ---- */
    if (app_count > 0) {
        /* Build a CFS image with app files injected */
        cfs_image_t *cfs = malloc(sizeof(cfs_image_t));
        if (!cfs) {
            fprintf(stderr, "Error: out of memory for CFS image\n");
            cybiko_destroy(emu);
            if (audio_dev) SDL_CloseAudioDevice(audio_dev);
            SDL_DestroyTexture(texture);
            SDL_DestroyRenderer(renderer);
            SDL_DestroyWindow(window);
            SDL_Quit();
            free(boot_data);
            free(flash_data);
            free(nvram_data);
            return 1;
        }

        if (nvram_data && nvram_size >= CFS_IMAGE_SIZE) {
            /* Load existing NVRAM as CFS image */
            memcpy(cfs->data, nvram_data, CFS_IMAGE_SIZE);
        } else {
            /* Format a fresh CFS filesystem */
            cfs_format(cfs);
            if (nvram_data) {
                fprintf(stderr, "Note: NVRAM too small for CFS, formatting fresh\n");
            }
        }

        /* Add each app file */
        for (int i = 0; i < app_count; i++) {
            size_t app_size = 0;
            uint8_t *app_data = load_file(app_paths[i], &app_size);
            if (!app_data) {
                fprintf(stderr, "Warning: skipping app '%s'\n", app_paths[i]);
                continue;
            }

            /* Extract filename from path */
            const char *app_name = strrchr(app_paths[i], '/');
            app_name = app_name ? app_name + 1 : app_paths[i];

            if (cfs_add_file(cfs, app_name, app_data, app_size)) {
                fprintf(stderr, "Added app '%s' (%zu bytes)\n", app_name, app_size);
            } else {
                fprintf(stderr, "Warning: failed to add app '%s' (filesystem full?)\n",
                        app_name);
            }
            free(app_data);
        }

        /* Load CFS image into external RAM */
        cybiko_load_nvram(emu, cfs->data, CFS_IMAGE_SIZE);
        free(cfs);
    } else if (nvram_data) {
        if (!cybiko_load_nvram(emu, nvram_data, nvram_size)) {
            fprintf(stderr, "Warning: failed to load NVRAM data\n");
        }
    }

    /* ROM data has been copied into the emulator - free the buffers */
    free(boot_data);
    free(flash_data);
    free(nvram_data);

    /* ---- Start emulation ---- */
    cybiko_reset(emu);

    /* Unpause audio device to start playback */
    if (audio_dev) {
        SDL_PauseAudioDevice(audio_dev, 0);
    }

    /* ---- Main loop ---- */
    bool running = true;
    uint32_t radio_report = SDL_GetTicks() + 10000;
    while (running) {
        uint32_t frame_start = SDL_GetTicks();

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
                case SDL_QUIT:
                    running = false;
                    break;
                case SDL_KEYDOWN:
                case SDL_KEYUP:
                    update_key_matrix(&sdl_ctx, &event.key);
                    break;
                default:
                    break;
            }
        }

        keyboard_tick_frame(&sdl_ctx);
        cybiko_run_frame(emu);
        if (radio_on) {
            radio_link.base.pump(&radio_link.base);
            if ((int32_t)(SDL_GetTicks() - radio_report) >= 0) {
                radio_print_stats(&radio_link, &radio_phy);
                radio_report += 10000;
            }
        }

        /* Frame-rate limiting (~60 FPS) */
        uint32_t elapsed = SDL_GetTicks() - frame_start;
        if (elapsed < 16) {
            SDL_Delay(16 - elapsed);
        }
    }

    /* ---- Save NVRAM if path was provided ---- */
    if (nvram_path) {
        size_t nvram_len = 0;
        const uint8_t *nvram = cybiko_get_nvram(emu, &nvram_len);
        if (nvram && nvram_len > 0) {
            FILE *f = fopen(nvram_path, "wb");
            if (f) {
                fwrite(nvram, 1, nvram_len, f);
                fclose(f);
                fprintf(stderr, "NVRAM saved to '%s' (%zu bytes)\n",
                        nvram_path, nvram_len);
            } else {
                fprintf(stderr, "Warning: could not save NVRAM to '%s'\n",
                        nvram_path);
            }
        }
    }

    /* ---- Cleanup ---- */
    if (radio_on) {
        radio_print_stats(&radio_link, &radio_phy);
        cybiko_serial_phy_close(&radio_phy);
    }
    cybiko_destroy(emu);
    if (audio_dev) SDL_CloseAudioDevice(audio_dev);
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}
