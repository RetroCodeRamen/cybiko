#ifndef CYBIKO_HAL_H
#define CYBIKO_HAL_H
#include "../core/types.h"

typedef struct {
    void (*render_frame)(void *ctx, const uint8_t *pixels, int width, int height);
    void (*audio_output)(void *ctx, const uint8_t *samples, int count);
    void (*keyboard_poll)(void *ctx, uint16_t *matrix, int num_columns);
    void (*serial_output)(void *ctx, uint8_t byte);
    void *ctx;
} cybiko_hal_t;

#endif
