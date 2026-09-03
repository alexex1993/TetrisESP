/*
 * Touch gestures -> game actions.
 *
 * There is no keypad on this board, so the whole control scheme is the panel.
 * It follows what mobile Tetris players already know:
 *
 *   drag left/right   move the piece one column per 12 px of travel
 *   tap left half     rotate counter-clockwise
 *   tap right half    rotate clockwise
 *   drag down         soft drop, one row per 12 px
 *   flick down        hard drop
 *   flick up          hold
 *   tap the HUD       pause
 *
 * A flick is told from a drag by speed, not distance, so it fires on the frame
 * the finger moves fast rather than after a threshold the slow drag would also
 * cross on its way.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "axs5106l.h"

typedef enum {
    IN_LEFT = 0,
    IN_RIGHT,
    IN_ROT_CW,
    IN_ROT_CCW,
    IN_SOFT_DROP,
    IN_HARD_DROP,
    IN_HOLD,
    IN_PAUSE,       /*!< tap inside the status strip */
    IN_ANY_TAP      /*!< any completed tap, for the title / game-over screens */
} input_ev_t;

#define IN_MAX_EVENTS 12

typedef struct {
    input_ev_t ev[IN_MAX_EVENTS];
    uint8_t n;
} input_events_t;

void input_init(axs5106l_handle_t tp);

/** @brief Poll the controller once and turn this sample into events. */
void input_poll(input_events_t *out, int64_t now_us);

/** @brief Abandon the gesture in progress (after a screen change). */
void input_reset(void);

/** @brief True while a finger is on the panel. */
bool input_touching(void);
