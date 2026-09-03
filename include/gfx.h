/*
 * Tiny tile renderer for the 172x320 JD9853 panel.
 *
 * There is no framebuffer: 110 KB of DRAM would buy nothing when the SPI bus
 * caps a full-screen flush at 22 ms anyway. Instead every repaint is a
 * rectangle, rendered band by band into one small DMA staging buffer by a
 * painter callback and pushed straight to the panel.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "board_pins.h"

#define GFX_W          BSP_LCD_H_RES     /* 172 */
#define GFX_H          BSP_LCD_V_RES     /* 320 */
#define GFX_TILE_ROWS  32                /* staging buffer height, in pixels */

/* The panel takes RGB565 big-endian and the CPU is little-endian, so every
 * colour is pre-swapped at compile time: the blitters stay dumb and no
 * per-pixel byte swap ever runs inside a redraw. */
#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
#define COLOR(r, g, b)  ((uint16_t)((RGB565(r, g, b) << 8) | (RGB565(r, g, b) >> 8)))

/** @brief One band of a repaint. Coordinates passed to the primitives below
 *         are screen coordinates; the canvas clips them to its own rectangle. */
typedef struct {
    int x0, y0;       /*!< band origin on screen */
    int w, h;         /*!< band size */
    uint16_t *buf;    /*!< w*h pixels, row-major */
} canvas_t;

typedef void (*gfx_painter_t)(canvas_t *c, void *ctx);

/** @brief Bring up SPI2, the JD9853 panel and the DMA staging buffer. */
void gfx_init(void);

/** @brief Backlight duty, 0..100 %. */
void gfx_backlight(uint8_t percent);

/**
 * @brief Repaint one screen rectangle.
 *
 * Splits it into bands that fit the staging buffer, calls @p painter for each
 * and pushes it. The rectangle is clipped to the screen first.
 */
void gfx_region(int x, int y, int w, int h, gfx_painter_t painter, void *ctx);

/** @brief Runtime colour helper, for palettes derived at start-up. */
uint16_t gfx_rgb(int r, int g, int b);

void gfx_pixel(canvas_t *c, int x, int y, uint16_t color);
void gfx_fill(canvas_t *c, int x, int y, int w, int h, uint16_t color);
void gfx_frame(canvas_t *c, int x, int y, int w, int h, int thickness, uint16_t color);
void gfx_char(canvas_t *c, int x, int y, char ch, int scale, uint16_t color);
void gfx_text(canvas_t *c, int x, int y, const char *s, int scale, uint16_t color);
void gfx_text_center(canvas_t *c, int y, const char *s, int scale, uint16_t color);
int  gfx_text_w(const char *s, int scale);
