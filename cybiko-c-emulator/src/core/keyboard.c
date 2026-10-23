/*
 * Cybiko keyboard matrix emulation.
 *
 * The keyboard is a matrix of columns, read via memory-mapped I/O
 * at address range 0xE00000. Each column has a 16-bit bitmask of
 * pressed keys.
 */
#include "core/keyboard.h"
#include <string.h>

void keyboard_init(keyboard_t *kb, int num_columns) {
    memset(kb, 0, sizeof(*kb));
    kb->num_columns = num_columns;
    if (kb->num_columns > KEYBOARD_MAX_COLUMNS) {
        kb->num_columns = KEYBOARD_MAX_COLUMNS;
    }
}

uint16_t keyboard_read(const keyboard_t *kb, uint32_t address) {
    uint32_t word_offset = ((address - 0xE00000) & 0xFFFFF) >> 1;
    uint16_t data = 0xFFFF;
    for (int i = 0; i < kb->num_columns; i++) {
        if ((word_offset & (1 << i)) == 0) {
            data &= ~kb->columns[i];
        }
    }
    return data;
}

void keyboard_set_key(keyboard_t *kb, int column, uint16_t bitmask, bool pressed) {
    if (column < 0 || column >= kb->num_columns) return;
    if (pressed) {
        kb->columns[column] |= bitmask;
    } else {
        kb->columns[column] &= ~bitmask;
    }
}

void keyboard_set_matrix(keyboard_t *kb, const uint16_t *matrix) {
    for (int i = 0; i < kb->num_columns; i++) {
        kb->columns[i] = matrix[i];
    }
}
