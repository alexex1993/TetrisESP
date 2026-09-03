/*
 * Tetris rules, per the Tetris Guideline.  No display, no input, no board:
 * this half is portable C and is what the renderer reads from.
 *
 * Implemented from the Guideline as documented on tetris.wiki:
 *   - 10x20 matrix with a hidden buffer above it,
 *   - Super Rotation System with the standard wall-kick tables
 *     (one set for J/L/S/T/Z, its own set for I, no kicks for O),
 *   - 7-bag Random Generator, one-piece hold, ghost piece,
 *   - 0.5 s lock delay with move reset, limited to 15 resets,
 *   - the Tetris Worlds speed curve,  Time = (0.8 - (L-1)*0.007)^(L-1).
 *
 * Scoring is deliberately NOT the guideline's. It is the original table --
 * 40 / 100 / 300 / 1200 x level for one to four lines -- so points come only
 * from clearing lines and a tetris is worth as much as thirty singles. No
 * points for dropping a piece, no combo, no back-to-back, no perfect-clear
 * bonus: dropping pieces fast must not inflate the score.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define T_COLS       10
#define T_ROWS       20                     /*!< visible rows */
#define T_HIDDEN     4                      /*!< buffer rows above the matrix */
#define T_TOTAL      (T_ROWS + T_HIDDEN)

#define T_QUEUE_CAP  14                     /*!< two bags */
#define T_LOCK_DELAY_US   500000            /*!< 0.5 s, per the Guideline */
#define T_LOCK_RESET_MAX  15                /*!< extended placement lock down */

/* Animation timings. A hard drop travels down the matrix instead of teleporting,
 * and a cleared row flashes white and then burns away from the middle out. */
#define T_DROP_ROW_US      4500             /*!< hard drop: one row, ~220 rows/s */
#define T_CLEAR_FLASH_US   60000            /*!< the white flash */
#define T_CLEAR_WIPE_US    180000           /*!< then the centre-out wipe */
#define T_CLEAR_WIPE_STEPS 5                /*!< cell pairs removed per row */
#define T_CLEAR_TOTAL_US   (T_CLEAR_FLASH_US + T_CLEAR_WIPE_US)

typedef enum {
    P_I = 0, P_J, P_L, P_O, P_S, P_T, P_Z,
    P_COUNT,
    P_NONE = 0x7F
} piece_t;

typedef enum {
    TS_FALLING = 0,   /*!< a piece is in play */
    TS_DROPPING,      /*!< a hard drop is travelling down; input is ignored */
    TS_CLEARING,      /*!< full rows are burning away, nothing is controllable */
    TS_OVER           /*!< topped out */
} tstate_t;

typedef struct {
    /* grid[0] is the top buffer row; visible rows are T_HIDDEN..T_TOTAL-1.
     * 0 = empty, otherwise piece_t + 1. */
    uint8_t  grid[T_TOTAL][T_COLS];

    tstate_t state;

    piece_t  cur;          /*!< falling piece */
    int8_t   rot;          /*!< 0 = spawn, 1 = R (cw), 2 = 180, 3 = L (ccw) */
    int8_t   px, py;       /*!< top-left of the piece's 4x4 box, in grid cells */

    piece_t  queue[T_QUEUE_CAP];
    uint8_t  q_head, q_len;

    piece_t  hold;
    bool     hold_used;    /*!< hold is one per piece */

    uint32_t score;
    uint32_t lines;
    uint16_t level;
    uint16_t start_level;

    int8_t   drop_to;      /*!< row a hard drop is travelling to */
    int8_t   trail_py;     /*!< where it was at the start of this frame */

    int64_t  gravity_us;   /*!< time for one row at the current level */
    int64_t  fall_acc;     /*!< doubles as the hard-drop animation clock */
    int64_t  lock_acc;
    bool     landed;
    uint8_t  lock_resets;

    bool     last_was_rotation;  /*!< for T-spin detection */
    uint8_t  last_kick;          /*!< which of the 5 kick tests succeeded */

    uint8_t  clear_rows[4];      /*!< rows flashing during TS_CLEARING */
    uint8_t  clear_count;
    int64_t  clear_acc;

    char     msg[24];      /*!< "TETRIS", "T-SPIN DOUBLE", ... */
    int64_t  msg_us_left;
} tetris_t;

/** @brief Start a new game at @p start_level (1..20). */
void tetris_reset(tetris_t *g, int start_level);

/** @brief Advance gravity, lock delay and the clear animation by @p dt_us. */
void tetris_tick(tetris_t *g, int64_t dt_us);

/** @brief Shift the piece by @p dx columns. @return true if it moved. */
bool tetris_move(tetris_t *g, int dx);

/** @brief Rotate: @p dir +1 clockwise, -1 counter-clockwise, with wall kicks. */
bool tetris_rotate(tetris_t *g, int dir);

/** @brief One row down. @return true if it moved. */
bool tetris_soft_drop(tetris_t *g);

/** @brief Start a hard drop. The piece travels down over the next few frames
 *         (TS_DROPPING) and locks when it arrives; a piece already resting
 *         locks at once. Dropping scores nothing. */
void tetris_hard_drop(tetris_t *g);

/** @brief Swap the falling piece with the hold slot. @return true if allowed. */
bool tetris_hold(tetris_t *g);

/** @brief Row the piece would land on, as a @c py value (for the ghost). */
int tetris_ghost_py(const tetris_t *g);

/** @brief The 4x4 occupancy mask of a piece in a rotation; bit i = row i/4, col i%4. */
uint16_t tetris_shape(piece_t p, int rot);

/** @brief Next piece in the queue, or P_NONE. */
piece_t tetris_peek(const tetris_t *g, int ahead);
