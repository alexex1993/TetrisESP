#include "render.h"

#include <stdio.h>
#include <string.h>

#include "gfx.h"

/* ------------------------------------------------------------------ */
/*  Palette                                                            */
/* ------------------------------------------------------------------ */

#define C_BG      COLOR(0x0B, 0x0E, 0x18)   /* outside the matrix */
#define C_FIELD   COLOR(0x14, 0x18, 0x26)   /* an empty cell */
#define C_GRID    COLOR(0x1E, 0x24, 0x36)   /* the 1 px lattice */
#define C_FRAME   COLOR(0x39, 0x44, 0x60)
#define C_TEXT    COLOR(0xF2, 0xF4, 0xF8)
#define C_MUTED   COLOR(0x76, 0x82, 0x9C)
#define C_ACCENT  COLOR(0x2E, 0xC4, 0xB6)
#define C_FLASH   COLOR(0xFF, 0xFF, 0xFF)
#define C_BANNER  COLOR(0x1B, 0x22, 0x38)
#define C_WARN    COLOR(0xFF, 0xC4, 0x4D)

/* Display-cell codes, one byte per matrix cell. */
#define DC_EMPTY   0x00
#define DC_BLOCK   0x00   /* 1..7 = piece + 1 */
#define DC_GHOST   0x10   /* 0x11..0x17 */
#define DC_FLASHED 0x20
#define DC_TRAIL   0x40   /* 0x41..0x47: the streak a hard drop leaves behind */
#define DC_DIRTY   0xFF   /* never produced; forces a repaint */

/* Base RGB per piece, in the guideline's shape-to-colour mapping. */
static const uint8_t PIECE_RGB[P_COUNT][3] = {
    /* I */ {   0, 226, 232 },
    /* J */ {  42,  92, 238 },
    /* L */ { 240, 146,  20 },
    /* O */ { 238, 214,  38 },
    /* S */ {  38, 208,  84 },
    /* T */ { 172,  62, 232 },
    /* Z */ { 236,  56,  76 },
};

static uint16_t s_base[P_COUNT], s_light[P_COUNT], s_dark[P_COUNT], s_ghost[P_COUNT];

/* ------------------------------------------------------------------ */
/*  Frame-to-frame state                                               */
/* ------------------------------------------------------------------ */

#define MINI 6   /* cell size in the hold/next previews */

/* How much matrix a single frame may repaint.
 *
 * Measured on the board: a 14x14 cell is 392 bytes, which is 78 us on a 40 MHz
 * bus, and every esp_lcd_panel_draw_bitmap() costs about another 450 us in
 * address commands and the DMA round trip.  A whole 200-cell matrix is 15.7 ms
 * of pure wire time and cannot fit in a 16.7 ms frame however it is sliced.
 *
 * So the budget is spent in cell-equivalents, and a repaint is charged for its
 * draw_bitmap as well as its pixels -- 450 us is worth about six cells.  Under
 * this budget the worst frame measured on the board is 15 ms whatever the
 * shape of the change:
 * a hard drop's streak touches many narrow rows, a line-clear collapse touches
 * few wide ones, and both come out the same.  Whatever does not fit stays
 * dirty and is picked up next frame, so no frame is ever dropped.
 */
#define REDRAW_BUDGET       105
#define REDRAW_REGION_COST  6

#define BANNER_Y  (PF_Y + 7 * CELL)
#define BANNER_H  22

static uint8_t s_disp[T_ROWS][T_COLS];   /* what this frame should show */
static uint8_t s_prev[T_ROWS][T_COLS];   /* what the panel is showing now */

static uint32_t s_hud_score;
static uint32_t s_hud_lines;
static uint16_t s_hud_level;
static int      s_hud_fps;
static piece_t  s_hud_next, s_hud_hold;
static bool     s_hud_hold_used;

static uint8_t s_scan_row;   /* where the next incremental repaint resumes */
static bool s_banner_on;
static char s_banner_text[24];

void render_init(void)
{
    for (int i = 0; i < P_COUNT; i++) {
        int r = PIECE_RGB[i][0], g = PIECE_RGB[i][1], b = PIECE_RGB[i][2];
        s_base[i]  = gfx_rgb(r, g, b);
        s_light[i] = gfx_rgb(r + (255 - r) * 45 / 100,
                             g + (255 - g) * 45 / 100,
                             b + (255 - b) * 45 / 100);
        s_dark[i]  = gfx_rgb(r * 40 / 100, g * 40 / 100, b * 40 / 100);
        s_ghost[i] = gfx_rgb(r * 55 / 100, g * 55 / 100, b * 55 / 100);
    }
}

/* ------------------------------------------------------------------ */
/*  Cells                                                              */
/* ------------------------------------------------------------------ */

static void draw_empty(canvas_t *c, int x, int y)
{
    gfx_fill(c, x, y, CELL, CELL, C_FIELD);
    gfx_fill(c, x, y, CELL, 1, C_GRID);
    gfx_fill(c, x, y, 1, CELL, C_GRID);
}

static void draw_cell(canvas_t *c, int col, int row, uint8_t v)
{
    int x = PF_X + col * CELL;
    int y = PF_Y + row * CELL;

    if (v == DC_FLASHED) {
        gfx_fill(c, x, y, CELL, CELL, C_FLASH);
    } else if (v == DC_EMPTY) {
        draw_empty(c, x, y);
    } else if (v & DC_TRAIL) {
        /* A narrow bar down the middle of the cell: motion blur, not a block. */
        draw_empty(c, x, y);
        gfx_fill(c, x + 4, y, CELL - 8, CELL, s_dark[(v & 7) - 1]);
    } else if (v & DC_GHOST) {
        draw_empty(c, x, y);
        gfx_frame(c, x + 2, y + 2, CELL - 4, CELL - 4, 2, s_ghost[(v & 7) - 1]);
    } else {
        int t = v - 1;
        gfx_fill(c, x, y, CELL, CELL, s_dark[t]);
        gfx_fill(c, x + 1, y + 1, CELL - 2, CELL - 2, s_base[t]);
        gfx_fill(c, x + 1, y + 1, CELL - 2, 2, s_light[t]);
        gfx_fill(c, x + 1, y + 1, 2, CELL - 2, s_light[t]);
    }
}

/* The whole matrix, for the one-off full repaints. */
static void paint_field(canvas_t *c, void *ctx)
{
    (void)ctx;
    /* Which rows this band touches. Integer division truncates toward zero, so
     * the negative case has to be clamped before dividing, not after. */
    int y0 = c->y0 - PF_Y;
    int y1 = c->y0 + c->h - 1 - PF_Y;
    if (y1 < 0 || y0 >= PF_H) {
        return;
    }
    int r0 = (y0 < 0) ? 0 : y0 / CELL;
    int r1 = y1 / CELL;
    if (r1 > T_ROWS - 1) { r1 = T_ROWS - 1; }

    for (int r = r0; r <= r1; r++) {
        for (int col = 0; col < T_COLS; col++) {
            draw_cell(c, col, r, s_disp[r][col]);
        }
    }
}

/* One horizontal span of a row: the unit of an incremental redraw. Two
 * separated changes in the same row are merged into one span -- the cells in
 * between cost 78 us each, far less than a second draw_bitmap. */
typedef struct { int row, col, n; } run_t;

static void paint_run(canvas_t *c, void *ctx)
{
    const run_t *r = ctx;
    for (int i = 0; i < r->n; i++) {
        draw_cell(c, r->col + i, r->row, s_disp[r->row][r->col + i]);
    }
}

static void draw_mini(canvas_t *c, int bx, int by, int bw, int bh, piece_t p, bool dim)
{
    gfx_fill(c, bx, by, bw, bh, C_BG);
    if (p >= P_COUNT) {
        return;
    }

    uint16_t m = tetris_shape(p, 0);
    int c0 = 3, c1 = 0, r0 = 3, r1 = 0;
    for (int i = 0; i < 16; i++) {
        if (!(m & (1u << i))) { continue; }
        int cc = i & 3, rr = i >> 2;
        if (cc < c0) { c0 = cc; }
        if (cc > c1) { c1 = cc; }
        if (rr < r0) { r0 = rr; }
        if (rr > r1) { r1 = rr; }
    }
    int ox = bx + (bw - (c1 - c0 + 1) * MINI) / 2;
    int oy = by + (bh - (r1 - r0 + 1) * MINI) / 2;

    for (int i = 0; i < 16; i++) {
        if (!(m & (1u << i))) { continue; }
        int x = ox + ((i & 3) - c0) * MINI;
        int y = oy + ((i >> 2) - r0) * MINI;
        gfx_fill(c, x, y, MINI, MINI, s_dark[p]);
        gfx_fill(c, x, y, MINI - 1, MINI - 1, dim ? s_ghost[p] : s_base[p]);
    }
}

/* ------------------------------------------------------------------ */
/*  Building the display grid                                          */
/* ------------------------------------------------------------------ */

static void build_disp(const tetris_t *g)
{
    for (int r = 0; r < T_ROWS; r++) {
        memcpy(s_disp[r], g->grid[r + T_HIDDEN], T_COLS);
    }

    if (g->state == TS_CLEARING) {
        /* The row flashes white, then burns away from the middle outwards --
         * one cell each side per step, five steps to empty. */
        int k = 0;
        if (g->clear_acc > T_CLEAR_FLASH_US) {
            k = (int)((g->clear_acc - T_CLEAR_FLASH_US) * T_CLEAR_WIPE_STEPS
                      / T_CLEAR_WIPE_US) + 1;
            if (k > T_CLEAR_WIPE_STEPS) { k = T_CLEAR_WIPE_STEPS; }
        }
        for (int i = 0; i < g->clear_count; i++) {
            int r = g->clear_rows[i] - T_HIDDEN;
            if (r < 0 || r >= T_ROWS) {
                continue;
            }
            for (int col = 0; col < T_COLS; col++) {
                bool gone = k > 0 && col >= T_COLS / 2 - k && col <= T_COLS / 2 - 1 + k;
                s_disp[r][col] = gone ? DC_EMPTY : DC_FLASHED;
            }
        }
        return;
    }
    if (g->state != TS_FALLING && g->state != TS_DROPPING) {
        return;
    }

    uint16_t m = tetris_shape(g->cur, g->rot);

    int gy = tetris_ghost_py(g);
    if (gy != g->py) {
        for (int i = 0; i < 16; i++) {
            if (!(m & (1u << i))) { continue; }
            int x = g->px + (i & 3);
            int y = gy + (i >> 2) - T_HIDDEN;
            if (y >= 0 && y < T_ROWS && x >= 0 && x < T_COLS && !s_disp[y][x]) {
                s_disp[y][x] = (uint8_t)(DC_GHOST | (g->cur + 1));
            }
        }
    }
    /* The streak between where the falling piece was when this frame started
     * and where it is now, so a hard drop reads as travel rather than a jump. */
    if (g->state == TS_DROPPING) {
        for (int i = 0; i < 16; i++) {
            if (!(m & (1u << i))) { continue; }
            int x = g->px + (i & 3);
            if (x < 0 || x >= T_COLS) { continue; }
            for (int t = g->trail_py; t < g->py; t++) {
                int y = t + (i >> 2) - T_HIDDEN;
                if (y >= 0 && y < T_ROWS && !s_disp[y][x]) {
                    s_disp[y][x] = (uint8_t)(DC_TRAIL | (g->cur + 1));
                }
            }
        }
    }

    for (int i = 0; i < 16; i++) {
        if (!(m & (1u << i))) { continue; }
        int x = g->px + (i & 3);
        int y = g->py + (i >> 2) - T_HIDDEN;
        if (y >= 0 && y < T_ROWS && x >= 0 && x < T_COLS) {
            s_disp[y][x] = (uint8_t)(g->cur + 1);
        }
    }
}

/* ------------------------------------------------------------------ */
/*  HUD                                                                */
/* ------------------------------------------------------------------ */

typedef struct { const tetris_t *g; int fps; } hud_ctx_t;

static void paint_score(canvas_t *c, void *ctx)
{
    const hud_ctx_t *h = ctx;
    char buf[16];
    uint32_t s = h->g->score;
    if (s > 9999999u) { s = 9999999u; }
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)s);

    gfx_fill(c, 0, 10, 90, 18, C_BG);
    gfx_text(c, 4, 11, buf, 2, C_TEXT);
}

static void paint_stats(canvas_t *c, void *ctx)
{
    const hud_ctx_t *h = ctx;
    char buf[24];
    snprintf(buf, sizeof(buf), "LV%02u LN%03u",
             (unsigned)(h->g->level > 99 ? 99 : h->g->level),
             (unsigned)(h->g->lines > 999 ? 999 : h->g->lines));

    gfx_fill(c, 0, 27, 92, 10, C_BG);
    gfx_text(c, 4, 28, buf, 1, C_MUTED);
}

static void paint_fps(canvas_t *c, void *ctx)
{
    const hud_ctx_t *h = ctx;
    char buf[8];
    int f = h->fps;
    if (f < 0) { f = 0; }
    if (f > 99) { f = 99; }
    snprintf(buf, sizeof(buf), "%2d", f);

    gfx_fill(c, 148, 27, 24, 10, C_BG);
    gfx_text(c, 152, 28, buf, 1, f >= 55 ? C_ACCENT : C_WARN);
}

static void paint_hold(canvas_t *c, void *ctx)
{
    const hud_ctx_t *h = ctx;
    draw_mini(c, 94, 11, 26, 16, h->g->hold, h->g->hold_used);
}

static void paint_next(canvas_t *c, void *ctx)
{
    const hud_ctx_t *h = ctx;
    draw_mini(c, 133, 11, 26, 16, tetris_peek(h->g, 0), false);
}

static void paint_banner(canvas_t *c, void *ctx)
{
    const char *text = ctx;
    gfx_fill(c, PF_X, BANNER_Y, PF_W, BANNER_H, C_BANNER);
    gfx_fill(c, PF_X, BANNER_Y, PF_W, 1, C_ACCENT);
    gfx_fill(c, PF_X, BANNER_Y + BANNER_H - 1, PF_W, 1, C_ACCENT);
    gfx_text_center(c, BANNER_Y + 7, text, 1, C_TEXT);
}

/* The entire screen in one pass. Painting the chrome and then the matrix as
 * two full-size regions would push 172x320 plus 140x280 over the bus -- 38 ms
 * of wire time for a repaint that only has to happen once per screen change. */
static void paint_all(canvas_t *c, void *ctx)
{
    gfx_fill(c, 0, 0, GFX_W, GFX_H, C_BG);

    gfx_text(c, 4, 2, "SCORE", 1, C_MUTED);
    gfx_text(c, 94, 2, "HOLD", 1, C_MUTED);
    gfx_text(c, 133, 2, "NEXT", 1, C_MUTED);
    gfx_frame(c, 93, 10, 28, 18, 1, C_FRAME);
    gfx_frame(c, 132, 10, 28, 18, 1, C_FRAME);

    gfx_frame(c, PF_X - 1, PF_Y - 1, PF_W + 2, PF_H + 2, 1, C_FRAME);

    paint_field(c, NULL);
    paint_score(c, ctx);
    paint_stats(c, ctx);
    paint_fps(c, ctx);
    paint_hold(c, ctx);
    paint_next(c, ctx);
}

/* ------------------------------------------------------------------ */
/*  Public                                                             */
/* ------------------------------------------------------------------ */

static void hud_update(const tetris_t *g, int fps, bool force)
{
    hud_ctx_t h = { .g = g, .fps = fps };

    if (force || g->score != s_hud_score) {
        s_hud_score = g->score;
        gfx_region(0, 10, 90, 18, paint_score, &h);
    }
    if (force || g->level != s_hud_level || g->lines != s_hud_lines) {
        s_hud_level = g->level;
        s_hud_lines = g->lines;
        gfx_region(0, 27, 92, 10, paint_stats, &h);
    }
    if (force || fps != s_hud_fps) {
        s_hud_fps = fps;
        gfx_region(148, 27, 24, 10, paint_fps, &h);
    }
    if (force || g->hold != s_hud_hold || g->hold_used != s_hud_hold_used) {
        s_hud_hold = g->hold;
        s_hud_hold_used = g->hold_used;
        gfx_region(94, 11, 26, 16, paint_hold, &h);
    }
    piece_t next = tetris_peek(g, 0);
    if (force || next != s_hud_next) {
        s_hud_next = next;
        gfx_region(133, 11, 26, 16, paint_next, &h);
    }
}

void render_full(const tetris_t *g, int fps)
{
    build_disp(g);

    hud_ctx_t h = { .g = g, .fps = fps };
    gfx_region(0, 0, GFX_W, GFX_H, paint_all, &h);

    memcpy(s_prev, s_disp, sizeof(s_prev));
    s_scan_row = 0;
    s_banner_on = false;
    s_banner_text[0] = '\0';

    /* paint_all already drew the HUD: just bring the shadow values in line so
     * the next incremental frame does not repaint it all over again. */
    s_hud_score = g->score;
    s_hud_level = g->level;
    s_hud_lines = g->lines;
    s_hud_fps = fps;
    s_hud_hold = g->hold;
    s_hud_hold_used = g->hold_used;
    s_hud_next = tetris_peek(g, 0);
}

void render_frame(const tetris_t *g, int fps)
{
    build_disp(g);

    bool want_banner = (g->msg[0] != '\0') && (g->msg_us_left > 0);
    bool text_changed = want_banner && strcmp(s_banner_text, g->msg) != 0;

    int b_row0 = (BANNER_Y - PF_Y) / CELL;
    int b_row1 = (BANNER_Y + BANNER_H - 1 - PF_Y) / CELL;
    if (b_row0 < 0) { b_row0 = 0; }
    if (b_row1 > T_ROWS - 1) { b_row1 = T_ROWS - 1; }

    /* The banner sits on top of the matrix, so when it goes the cells under it
     * have to come back: forget what they were showing. */
    if (!want_banner && s_banner_on) {
        for (int r = b_row0; r <= b_row1; r++) {
            memset(s_prev[r], DC_DIRTY, T_COLS);
        }
    }

    bool banner_hit = false;
    int budget = REDRAW_BUDGET;
    int r = s_scan_row;

    for (int k = 0; k < T_ROWS; k++, r = (r + 1) % T_ROWS) {
        if (memcmp(s_prev[r], s_disp[r], T_COLS) == 0) {
            continue;
        }
        if (budget <= 0) {
            break;      /* the rest stays dirty and is picked up next frame */
        }

        int a = 0, b = T_COLS - 1;
        while (s_prev[r][a] == s_disp[r][a]) { a++; }
        while (s_prev[r][b] == s_disp[r][b]) { b--; }

        run_t run = { .row = r, .col = a, .n = b - a + 1 };
        gfx_region(PF_X + a * CELL, PF_Y + r * CELL, run.n * CELL, CELL,
                   paint_run, &run);
        memcpy(s_prev[r], s_disp[r], T_COLS);
        budget -= run.n + REDRAW_REGION_COST;

        if (want_banner && r >= b_row0 && r <= b_row1) {
            banner_hit = true;
        }
    }
    /* Resume where the budget ran out, so no row can starve. */
    s_scan_row = r % T_ROWS;

    if (want_banner && (!s_banner_on || text_changed || banner_hit)) {
        snprintf(s_banner_text, sizeof(s_banner_text), "%s", g->msg);
        gfx_region(PF_X, BANNER_Y, PF_W, BANNER_H, paint_banner, s_banner_text);
    }
    if (!want_banner) {
        s_banner_text[0] = '\0';
    }
    s_banner_on = want_banner;

    hud_update(g, fps, false);
}

/* ------------------------------------------------------------------ */
/*  Full-screen overlays                                               */
/* ------------------------------------------------------------------ */

static void paint_title(canvas_t *c, void *ctx)
{
    uint32_t hiscore = *(const uint32_t *)ctx;
    char buf[24];

    gfx_fill(c, 0, 0, GFX_W, GFX_H, C_BG);

    /* A row of tetrominoes as a rule under the title. */
    for (int i = 0; i < P_COUNT; i++) {
        gfx_fill(c, 10 + i * 22, 96, 20, 6, s_dark[i]);
        gfx_fill(c, 10 + i * 22, 96, 19, 5, s_base[i]);
    }

    gfx_text_center(c, 52, "TETRIS", 3, C_TEXT);
    gfx_text_center(c, 82, "ESP32-C6 . 60 FPS", 1, C_ACCENT);

    gfx_text(c, 14, 126, "DRAG", 1, C_ACCENT);
    gfx_text(c, 74, 126, "MOVE", 1, C_MUTED);
    gfx_text(c, 14, 142, "TAP L/R", 1, C_ACCENT);
    gfx_text(c, 74, 142, "ROTATE", 1, C_MUTED);
    gfx_text(c, 14, 158, "FLICK DN", 1, C_ACCENT);
    gfx_text(c, 74, 158, "HARD DROP", 1, C_MUTED);
    gfx_text(c, 14, 174, "DRAG DN", 1, C_ACCENT);
    gfx_text(c, 74, 174, "SOFT DROP", 1, C_MUTED);
    gfx_text(c, 14, 190, "FLICK UP", 1, C_ACCENT);
    gfx_text(c, 74, 190, "HOLD", 1, C_MUTED);
    gfx_text(c, 14, 206, "TAP TOP", 1, C_ACCENT);
    gfx_text(c, 74, 206, "PAUSE", 1, C_MUTED);

    snprintf(buf, sizeof(buf), "BEST %lu", (unsigned long)hiscore);
    gfx_text_center(c, 240, buf, 1, C_MUTED);

    gfx_frame(c, 26, 264, GFX_W - 52, 28, 1, C_ACCENT);
    gfx_text_center(c, 274, "TAP TO START", 1, C_TEXT);
}

void render_title(uint32_t hiscore)
{
    gfx_region(0, 0, GFX_W, GFX_H, paint_title, &hiscore);
}

static void paint_pause(canvas_t *c, void *ctx)
{
    (void)ctx;
    gfx_fill(c, PF_X, 110, PF_W, 100, C_BANNER);
    gfx_frame(c, PF_X, 110, PF_W, 100, 2, C_ACCENT);
    gfx_text_center(c, 138, "PAUSED", 2, C_TEXT);
    gfx_text_center(c, 174, "TAP TO RESUME", 1, C_MUTED);
}

void render_pause(void)
{
    gfx_region(PF_X, 110, PF_W, 100, paint_pause, NULL);
    memset(s_prev, DC_DIRTY, sizeof(s_prev));   /* the overlay covered cells */
    s_banner_on = false;
}

typedef struct { const tetris_t *g; uint32_t hiscore; bool record; } over_ctx_t;

static void paint_over(canvas_t *c, void *ctx)
{
    const over_ctx_t *o = ctx;
    char buf[24];

    gfx_fill(c, PF_X, 92, PF_W, 136, C_BANNER);
    gfx_frame(c, PF_X, 92, PF_W, 136, 2, C_WARN);

    gfx_text_center(c, 106, "GAME", 2, C_TEXT);
    gfx_text_center(c, 126, "OVER", 2, C_TEXT);

    snprintf(buf, sizeof(buf), "%lu", (unsigned long)o->g->score);
    gfx_text_center(c, 154, buf, 2, C_ACCENT);

    snprintf(buf, sizeof(buf), "LV%u  %luLN", (unsigned)o->g->level,
             (unsigned long)o->g->lines);
    gfx_text_center(c, 176, buf, 1, C_MUTED);

    if (o->record) {
        gfx_text_center(c, 192, "NEW RECORD!", 1, C_WARN);
    } else {
        snprintf(buf, sizeof(buf), "BEST %lu", (unsigned long)o->hiscore);
        gfx_text_center(c, 192, buf, 1, C_MUTED);
    }
    gfx_text_center(c, 212, "TAP TO RESTART", 1, C_TEXT);
}

void render_gameover(const tetris_t *g, uint32_t hiscore, bool is_record)
{
    over_ctx_t o = { .g = g, .hiscore = hiscore, .record = is_record };
    gfx_region(PF_X, 92, PF_W, 136, paint_over, &o);
    memset(s_prev, DC_DIRTY, sizeof(s_prev));
    s_banner_on = false;
}
