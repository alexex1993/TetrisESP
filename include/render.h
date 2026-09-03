/*
 * Game -> screen.  The playfield is redrawn cell by cell against a shadow copy
 * of the previous frame, so a normal frame moves 8-16 cells (~1.5 ms on the
 * SPI bus) instead of the 22 ms a full-screen flush costs.  That is what makes
 * 60 fps possible on a bus that caps full redraws at ~45 fps.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "tetris.h"

/* Layout, 172x320 portrait: a 37 px status strip over a 10x20 matrix of
 * 14 px cells, framed, with 15 px margins left and right. */
#define HUD_H   37
#define CELL    14
#define PF_X    16
#define PF_Y    38
#define PF_W    (T_COLS * CELL)   /* 140 */
#define PF_H    (T_ROWS * CELL)   /* 280 */

void render_init(void);

/** @brief Repaint everything: background, frames, labels, matrix and HUD. */
void render_full(const tetris_t *g, int fps);

/** @brief Repaint only what changed since the previous call. */
void render_frame(const tetris_t *g, int fps);

void render_title(uint32_t hiscore);
void render_pause(void);
void render_gameover(const tetris_t *g, uint32_t hiscore, bool is_record);
