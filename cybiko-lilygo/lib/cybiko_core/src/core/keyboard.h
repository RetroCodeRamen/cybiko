#ifndef CYBIKO_KEYBOARD_H
#define CYBIKO_KEYBOARD_H
#include "types.h"

#define KEYBOARD_MAX_COLUMNS 15

typedef struct {
    uint16_t columns[KEYBOARD_MAX_COLUMNS];
    int      num_columns;
} keyboard_t;

void     keyboard_init(keyboard_t *kb, int num_columns);
uint16_t keyboard_read(const keyboard_t *kb, uint32_t address);
void     keyboard_set_key(keyboard_t *kb, int column, uint16_t bitmask, bool pressed);
void     keyboard_set_matrix(keyboard_t *kb, const uint16_t *matrix);

#endif
