#include "tetris.h"

#include <stdio.h>
#include <string.h>

#include "esp_random.h"

/* ------------------------------------------------------------------ */
/*  Piece shapes                                                       */
/* ------------------------------------------------------------------ */

/* Every piece lives in the same 4x4 box, bit i = (row i/4, col i%4), row 0 on
 * top. The 3-wide pieces sit in the top-left 3x3 of that box, so their
 * rotation centre is at box (1,1) in all four states -- which is exactly the
 * SRS "basic rotation" and lets the kick tables below apply unchanged.
 *
 * Spawn state 0 puts the flat side of J/L/T on box row 1 and the I on box
 * row 1 as well, so all seven pieces enter the matrix on the same row. */
static const uint16_t SHAPES[P_COUNT][4] = {
    /* I */ { 0x00F0, 0x4444, 0x0F00, 0x2222 },
    /* J */ { 0x0071, 0x0226, 0x0470, 0x0322 },
    /* L */ { 0x0074, 0x0622, 0x0170, 0x0223 },
    /* O */ { 0x0066, 0x0066, 0x0066, 0x0066 },
    /* S */ { 0x0036, 0x0462, 0x0360, 0x0231 },
    /* T */ { 0x0072, 0x0262, 0x0270, 0x0232 },
    /* Z */ { 0x0063, 0x0264, 0x0630, 0x0132 },
};

uint16_t tetris_shape(piece_t p, int rot)
{
    return SHAPES[p][rot & 3];
}

/* ------------------------------------------------------------------ */
/*  Wall kicks (tetris.wiki/Super_Rotation_System)                      */
/* ------------------------------------------------------------------ */

/* The wiki's convention is +y up; the grid's is +y down, so every dy is
 * negated here once, at the table, rather than at each use. Indexed
 * [from_state][0 = clockwise, 1 = counter-clockwise][test][x or y]. */
static const int8_t KICKS_JLSTZ[4][2][5][2] = {
    /* from 0 */ { { {0,0}, {-1,0}, {-1,-1}, {0, 2}, {-1, 2} },    /* 0->R */
                   { {0,0}, { 1,0}, { 1,-1}, {0, 2}, { 1, 2} } },  /* 0->L */
    /* from R */ { { {0,0}, { 1,0}, { 1, 1}, {0,-2}, { 1,-2} },    /* R->2 */
                   { {0,0}, { 1,0}, { 1, 1}, {0,-2}, { 1,-2} } },  /* R->0 */
    /* from 2 */ { { {0,0}, { 1,0}, { 1,-1}, {0, 2}, { 1, 2} },    /* 2->L */
                   { {0,0}, {-1,0}, {-1,-1}, {0, 2}, {-1, 2} } },  /* 2->R */
    /* from L */ { { {0,0}, {-1,0}, {-1, 1}, {0,-2}, {-1,-2} },    /* L->0 */
                   { {0,0}, {-1,0}, {-1, 1}, {0,-2}, {-1,-2} } },  /* L->2 */
};

static const int8_t KICKS_I[4][2][5][2] = {
    /* from 0 */ { { {0,0}, {-2,0}, { 1,0}, {-2, 1}, { 1,-2} },    /* 0->R */
                   { {0,0}, {-1,0}, { 2,0}, {-1,-2}, { 2, 1} } },  /* 0->L */
    /* from R */ { { {0,0}, {-1,0}, { 2,0}, {-1,-2}, { 2, 1} },    /* R->2 */
                   { {0,0}, { 2,0}, {-1,0}, { 2,-1}, {-1, 2} } },  /* R->0 */
    /* from 2 */ { { {0,0}, { 2,0}, {-1,0}, { 2,-1}, {-1, 2} },    /* 2->L */
                   { {0,0}, { 1,0}, {-2,0}, { 1, 2}, {-2,-1} } },  /* 2->R */
    /* from L */ { { {0,0}, { 1,0}, {-2,0}, { 1, 2}, {-2,-1} },    /* L->0 */
                   { {0,0}, {-2,0}, { 1,0}, {-2, 1}, { 1,-2} } },  /* L->2 */
};

/* ------------------------------------------------------------------ */
/*  Speed curve                                                        */
/* ------------------------------------------------------------------ */

/* Time = (0.8 - (Level-1)*0.007)^(Level-1) seconds per row, tabulated in
 * microseconds so no libm and no float ever enter the frame loop. Level 20 is
 * 36.6 G -- the piece reaches the stack the frame it appears -- and the table
 * is clamped there. */
static const int32_t GRAVITY_US[20] = {
    1000000, 793000, 617796, 472729, 355197, 262003, 189678, 134734,
      93882,  64151,  42976,  28215,  18153,  11439,   7059,   4263,
       2520,   1457,    824,    455,
};

static int64_t gravity_for(int level)
{
    if (level < 1) { level = 1; }
    if (level > 20) { level = 20; }
    return GRAVITY_US[level - 1];
}

/* ------------------------------------------------------------------ */
/*  Piece queue: the 7-bag Random Generator                            */
/* ------------------------------------------------------------------ */

static void bag_refill(tetris_t *g)
{
    piece_t bag[P_COUNT] = { P_I, P_J, P_L, P_O, P_S, P_T, P_Z };

    for (int i = P_COUNT - 1; i > 0; i--) {
        uint32_t j = esp_random() % (uint32_t)(i + 1);
        piece_t tmp = bag[i];
        bag[i] = bag[j];
        bag[j] = tmp;
    }
    for (int i = 0; i < P_COUNT; i++) {
        g->queue[(g->q_head + g->q_len + i) % T_QUEUE_CAP] = bag[i];
    }
    g->q_len += P_COUNT;
}

static piece_t queue_pop(tetris_t *g)
{
    if (g->q_len == 0) {
        bag_refill(g);
    }
    piece_t p = g->queue[g->q_head];
    g->q_head = (g->q_head + 1) % T_QUEUE_CAP;
    g->q_len--;
    if (g->q_len <= P_COUNT) {
        bag_refill(g);
    }
    return p;
}

piece_t tetris_peek(const tetris_t *g, int ahead)
{
    if (ahead >= g->q_len) {
        return P_NONE;
    }
    return g->queue[(g->q_head + ahead) % T_QUEUE_CAP];
}

/* ------------------------------------------------------------------ */
/*  Collision and placement                                            */
/* ------------------------------------------------------------------ */

static bool collides(const tetris_t *g, piece_t p, int rot, int px, int py)
{
    uint16_t m = SHAPES[p][rot & 3];

    for (int i = 0; i < 16; i++) {
        if (!(m & (1u << i))) {
            continue;
        }
        int x = px + (i & 3);
        int y = py + (i >> 2);
        if (x < 0 || x >= T_COLS || y >= T_TOTAL) {
            return true;
        }
        if (y >= 0 && g->grid[y][x]) {
            return true;
        }
    }
    return false;
}

int tetris_ghost_py(const tetris_t *g)
{
    int py = g->py;
    while (!collides(g, g->cur, g->rot, g->px, py + 1)) {
        py++;
    }
    return py;
}

/* Extended placement lock down: any successful move or rotation restarts the
 * 0.5 s timer, up to 15 times before the piece has to step down again. */
static void lock_reset_on_move(tetris_t *g)
{
    if (g->landed && g->lock_resets < T_LOCK_RESET_MAX) {
        g->lock_acc = 0;
        g->lock_resets++;
    }
}

static void spawn(tetris_t *g)
{
    g->state = TS_FALLING;   /* also the way out of TS_CLEARING */
    g->cur = queue_pop(g);
    g->rot = 0;
    g->px = 3;                 /* centred, rounded left, for every piece */
    g->py = T_HIDDEN;          /* the piece enters just inside the matrix */

    g->hold_used = false;
    g->landed = false;
    g->lock_acc = 0;
    g->lock_resets = 0;
    g->fall_acc = 0;
    g->last_was_rotation = false;
    g->last_kick = 0;

    /* Block out: the new piece overlaps the stack. */
    if (collides(g, g->cur, 0, g->px, g->py)) {
        g->state = TS_OVER;
    }
}

/* ------------------------------------------------------------------ */
/*  T-spin detection: the 3-corner rule                                */
/* ------------------------------------------------------------------ */

static bool corner_filled(const tetris_t *g, int x, int y)
{
    if (x < 0 || x >= T_COLS || y >= T_TOTAL) {
        return true;      /* walls and floor count as filled */
    }
    if (y < 0) {
        return false;     /* open sky does not */
    }
    return g->grid[y][x] != 0;
}

/* A T-spin is a T that was rotated into place with at least 3 of the 4 cells
 * diagonally around its centre occupied. It is a mini unless both corners on
 * the side the T points to are filled -- or the rotation only fit through the
 * 5th kick test, which always promotes it to a full T-spin. */
static void detect_tspin(const tetris_t *g, bool *tspin, bool *mini)
{
    *tspin = false;
    *mini = false;

    if (g->cur != P_T || !g->last_was_rotation) {
        return;
    }

    int cx = g->px + 1, cy = g->py + 1;
    bool c[4] = {                                     /* tl, tr, bl, br */
        corner_filled(g, cx - 1, cy - 1), corner_filled(g, cx + 1, cy - 1),
        corner_filled(g, cx - 1, cy + 1), corner_filled(g, cx + 1, cy + 1),
    };
    int filled = c[0] + c[1] + c[2] + c[3];
    if (filled < 3) {
        return;
    }

    /* The two corners the T's nub points between, per rotation state. */
    static const uint8_t FRONT[4][2] = { {0,1}, {1,3}, {2,3}, {0,2} };
    bool both_front = c[FRONT[g->rot & 3][0]] && c[FRONT[g->rot & 3][1]];

    *tspin = true;
    *mini = !both_front && g->last_kick < 4;
}

/* ------------------------------------------------------------------ */
/*  Locking, line clears and scoring                                   */
/* ------------------------------------------------------------------ */

static void set_msg(tetris_t *g, const char *s)
{
    snprintf(g->msg, sizeof(g->msg), "%s", s);
    g->msg_us_left = 900000;
}

static void lock_piece(tetris_t *g)
{
    bool tspin, mini;
    detect_tspin(g, &tspin, &mini);   /* for the banner; it does not score */

    uint16_t m = SHAPES[g->cur][g->rot & 3];
    bool all_hidden = true;

    for (int i = 0; i < 16; i++) {
        if (!(m & (1u << i))) {
            continue;
        }
        int x = g->px + (i & 3);
        int y = g->py + (i >> 2);
        if (y >= 0 && y < T_TOTAL) {
            g->grid[y][x] = (uint8_t)(g->cur + 1);
        }
        if (y >= T_HIDDEN) {
            all_hidden = false;
        }
    }

    /* Lock out: the piece came to rest entirely above the visible matrix. */
    if (all_hidden) {
        g->state = TS_OVER;
        return;
    }

    g->clear_count = 0;
    for (int y = 0; y < T_TOTAL; y++) {
        int n = 0;
        for (int x = 0; x < T_COLS; x++) {
            n += g->grid[y][x] ? 1 : 0;
        }
        if (n == T_COLS && g->clear_count < 4) {
            g->clear_rows[g->clear_count++] = (uint8_t)y;
        }
    }
    int lines = g->clear_count;

    /* A perfect clear leaves nothing behind: every occupied cell has to be in
     * a row that is about to go. */
    bool perfect = lines > 0;
    for (int y = 0; y < T_TOTAL && perfect; y++) {
        bool clearing = false;
        for (int i = 0; i < lines; i++) {
            clearing |= (g->clear_rows[i] == y);
        }
        if (clearing) {
            continue;
        }
        for (int x = 0; x < T_COLS; x++) {
            if (g->grid[y][x]) { perfect = false; break; }
        }
    }

    /* The original scoring table: points come only from clearing lines, and
     * a tetris is worth thirty times a single. Nothing is awarded for
     * dropping a piece, for combos, for back-to-back or for a perfect clear,
     * so a fast player scores by clearing well rather than by dropping often. */
    static const uint16_t LINE_SCORE[5] = { 0, 40, 100, 300, 1200 };
    g->score += (uint32_t)LINE_SCORE[lines] * g->level;

    /* Recognition only -- the banner names what happened, it does not score. */
    if (perfect) {
        set_msg(g, "PERFECT CLEAR");
    } else if (tspin) {
        static const char *N[4] = { "", " SINGLE", " DOUBLE", " TRIPLE" };
        char buf[24];
        snprintf(buf, sizeof(buf), "%sT-SPIN%s", mini ? "MINI " : "",
                 lines == 0 ? "" : N[lines]);
        set_msg(g, buf);
    } else if (lines == 4) {
        set_msg(g, "TETRIS");
    }

    if (lines > 0) {
        g->state = TS_CLEARING;
        g->clear_acc = 0;
    } else {
        spawn(g);
    }
}

static void collapse(tetris_t *g)
{
    for (int i = 0; i < g->clear_count; i++) {
        int row = g->clear_rows[i];
        for (int y = row; y > 0; y--) {
            memcpy(g->grid[y], g->grid[y - 1], T_COLS);
        }
        memset(g->grid[0], 0, T_COLS);
    }

    g->lines += g->clear_count;
    g->clear_count = 0;

    /* Fixed goal: ten lines per level, counted from the starting level. */
    uint16_t level = (uint16_t)(g->start_level + g->lines / 10);
    if (level > 99) {
        level = 99;
    }
    g->level = level;
    g->gravity_us = gravity_for(level);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

void tetris_reset(tetris_t *g, int start_level)
{
    if (start_level < 1) { start_level = 1; }
    if (start_level > 20) { start_level = 20; }

    memset(g, 0, sizeof(*g));
    g->state = TS_FALLING;
    g->hold = P_NONE;
    g->start_level = (uint16_t)start_level;
    g->level = (uint16_t)start_level;
    g->gravity_us = gravity_for(start_level);

    bag_refill(g);
    spawn(g);
}

bool tetris_move(tetris_t *g, int dx)
{
    if (g->state != TS_FALLING || collides(g, g->cur, g->rot, g->px + dx, g->py)) {
        return false;
    }
    g->px += dx;
    g->last_was_rotation = false;
    lock_reset_on_move(g);
    return true;
}

bool tetris_rotate(tetris_t *g, int dir)
{
    if (g->state != TS_FALLING || g->cur == P_O) {
        return false;
    }

    int from = g->rot & 3;
    int to = (from + (dir > 0 ? 1 : 3)) & 3;
    int which = (dir > 0) ? 0 : 1;
    const int8_t (*kick)[2] = (g->cur == P_I) ? KICKS_I[from][which]
                                              : KICKS_JLSTZ[from][which];

    for (int t = 0; t < 5; t++) {
        int nx = g->px + kick[t][0];
        int ny = g->py + kick[t][1];
        if (collides(g, g->cur, to, nx, ny)) {
            continue;
        }
        g->rot = (int8_t)to;
        g->px = (int8_t)nx;
        g->py = (int8_t)ny;
        g->last_was_rotation = true;
        g->last_kick = (uint8_t)t;
        lock_reset_on_move(g);
        return true;
    }
    return false;
}

bool tetris_soft_drop(tetris_t *g)
{
    if (g->state != TS_FALLING || collides(g, g->cur, g->rot, g->px, g->py + 1)) {
        return false;
    }
    g->py++;
    g->fall_acc = 0;
    g->landed = false;
    g->last_was_rotation = false;
    return true;
}

void tetris_hard_drop(tetris_t *g)
{
    if (g->state != TS_FALLING) {
        return;
    }

    int target = tetris_ghost_py(g);
    if (target == g->py) {
        lock_piece(g);       /* already resting: nothing to animate */
        return;
    }

    /* Travel there over the next few frames instead of teleporting. The piece
     * is committed from here on: tetris_move(), _rotate() and _soft_drop() all
     * refuse while the state is TS_DROPPING. */
    g->drop_to = (int8_t)target;
    g->trail_py = g->py;
    g->fall_acc = 0;
    g->last_was_rotation = false;
    g->state = TS_DROPPING;
}

bool tetris_hold(tetris_t *g)
{
    if (g->state != TS_FALLING || g->hold_used) {
        return false;
    }
    piece_t swap = g->hold;
    g->hold = g->cur;

    if (swap == P_NONE) {
        spawn(g);
    } else {
        g->cur = swap;
        g->rot = 0;
        g->px = 3;
        g->py = T_HIDDEN;
        g->landed = false;
        g->lock_acc = 0;
        g->lock_resets = 0;
        g->fall_acc = 0;
        g->last_was_rotation = false;
        if (collides(g, g->cur, 0, g->px, g->py)) {
            g->state = TS_OVER;
        }
    }
    g->hold_used = true;   /* set after spawn(), which clears it */
    return true;
}

void tetris_tick(tetris_t *g, int64_t dt_us)
{
    if (g->msg_us_left > 0) {
        g->msg_us_left -= dt_us;
        if (g->msg_us_left <= 0) {
            g->msg_us_left = 0;
            g->msg[0] = '\0';
        }
    }

    if (g->state == TS_CLEARING) {
        g->clear_acc += dt_us;
        if (g->clear_acc >= T_CLEAR_TOTAL_US) {
            collapse(g);
            spawn(g);
        }
        return;
    }

    if (g->state == TS_DROPPING) {
        /* trail_py is where the piece was when this frame started: the
         * renderer streaks the cells between there and where it is now. */
        g->trail_py = g->py;
        g->fall_acc += dt_us;
        while (g->fall_acc >= T_DROP_ROW_US && g->py < g->drop_to) {
            g->fall_acc -= T_DROP_ROW_US;
            g->py++;
        }
        if (g->py >= g->drop_to) {
            lock_piece(g);
        }
        return;
    }

    if (g->state != TS_FALLING) {
        return;
    }

    g->fall_acc += dt_us;
    while (g->fall_acc >= g->gravity_us) {
        if (collides(g, g->cur, g->rot, g->px, g->py + 1)) {
            g->fall_acc = 0;     /* resting on the stack; the lock timer runs */
            break;
        }
        g->fall_acc -= g->gravity_us;
        g->py++;
        g->last_was_rotation = false;
        g->landed = false;
        g->lock_resets = 0;      /* a step down clears the reset budget */
    }

    if (collides(g, g->cur, g->rot, g->px, g->py + 1)) {
        if (!g->landed) {
            g->landed = true;
            g->lock_acc = 0;
        }
        g->lock_acc += dt_us;
        if (g->lock_acc >= T_LOCK_DELAY_US) {
            lock_piece(g);
        }
    } else {
        g->landed = false;
    }
}
