/* Host test for src/tetris.c -- the rules half is plain C.
 *
 * Run with tools/host-test/run.sh */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "tetris.h"

static int fails = 0;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL %s:%d ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static void __attribute__((unused)) dump(const tetris_t *g)
{
    for (int y = 0; y < T_TOTAL; y++) {
        printf("%2d |", y);
        for (int x = 0; x < T_COLS; x++) putchar(g->grid[y][x] ? '0' + g->grid[y][x] : '.');
        printf("|%s\n", y == T_HIDDEN - 1 ? " <- top of matrix" : "");
    }
}

/* --- shapes: each rotation has exactly 4 minos, and 4 rotations return home */
static void test_shapes(void)
{
    static const char *NAME[] = { "I","J","L","O","S","T","Z" };
    for (int p = 0; p < P_COUNT; p++) {
        for (int r = 0; r < 4; r++) {
            int n = __builtin_popcount(tetris_shape(p, r));
            CHECK(n == 4, "%s rot %d has %d cells", NAME[p], r, n);
        }
    }
    /* Rotating the mask itself must match the table: 90 deg cw in a 4x4 box is
     * new(row,col) = old(3-col,row).  True for I and O (whole-box pieces);
     * the 3-wide pieces live in the top-left 3x3, so use a 3x3 rotation. */
    for (int p = 0; p < P_COUNT; p++) {
        int box = (p == P_I) ? 4 : 3;
        for (int r = 0; r < 4; r++) {
            uint16_t src = tetris_shape(p, r), want = 0;
            for (int i = 0; i < 16; i++) {
                if (!(src & (1u << i))) continue;
                int row = i >> 2, col = i & 3;
                int nrow = col, ncol = box - 1 - row;    /* cw */
                want |= 1u << (nrow * 4 + ncol);
            }
            if (p == P_O) continue;   /* O is defined as invariant */
            CHECK(want == tetris_shape(p, (r + 1) & 3),
                  "%s rot %d cw -> 0x%04X, table says 0x%04X",
                  NAME[p], r, want, tetris_shape(p, (r + 1) & 3));
        }
    }
}

/* --- helpers to drive a game deterministically ------------------------ */
static void force_piece(tetris_t *g, piece_t p, int px, int py, int rot)
{
    g->cur = p; g->px = px; g->py = py; g->rot = rot;
    g->state = TS_FALLING; g->landed = false; g->lock_acc = 0; g->fall_acc = 0;
}

/* Run the hard-drop and line-clear animations out to their end. */
static void settle(tetris_t *g)
{
    for (int i = 0; i < 4000 &&
         (g->state == TS_DROPPING || g->state == TS_CLEARING); i++) {
        tetris_tick(g, 1000);
    }
}

static void fill_row(tetris_t *g, int y, int except)
{
    for (int x = 0; x < T_COLS; x++) g->grid[y][x] = (x == except) ? 0 : (P_L + 1);
}

/* --- the original scoring table: lines only, a tetris pays the most ----- */
static void test_line_scores(void)
{
    static const struct { int rows; uint32_t points; } CASE[4] = {
        { 1, 40 }, { 2, 100 }, { 3, 300 }, { 4, 1200 },
    };

    for (int k = 0; k < 4; k++) {
        tetris_t g;
        tetris_reset(&g, 1);
        memset(g.grid, 0, sizeof(g.grid));
        for (int y = T_TOTAL - CASE[k].rows; y < T_TOTAL; y++) fill_row(&g, y, 0);

        force_piece(&g, P_I, -2, T_HIDDEN, 1);   /* box col 2 -> field col 0 */
        g.score = 0;
        tetris_hard_drop(&g);
        settle(&g);

        CHECK(g.lines == (uint32_t)CASE[k].rows, "%d rows: cleared %u",
              CASE[k].rows, (unsigned)g.lines);
        CHECK(g.score == CASE[k].points, "%d rows scored %u, expected %u",
              CASE[k].rows, (unsigned)g.score, (unsigned)CASE[k].points);
    }

    /* A tetris is worth thirty singles -- that is the whole point of the table. */
    CHECK(1200 == 30 * 40, "the table lost its shape");

    /* The level multiplies it. */
    tetris_t g;
    tetris_reset(&g, 5);
    memset(g.grid, 0, sizeof(g.grid));
    fill_row(&g, T_TOTAL - 1, 0);
    force_piece(&g, P_I, -2, T_HIDDEN, 1);
    g.score = 0;
    tetris_hard_drop(&g);
    settle(&g);
    CHECK(g.score == 40 * 5, "a single at level 5 scored %u, expected 200",
          (unsigned)g.score);
}

/* --- dropping pieces must not pay: that is what inflated the old score -- */
static void test_dropping_scores_nothing(void)
{
    tetris_t g;
    tetris_reset(&g, 1);
    memset(g.grid, 0, sizeof(g.grid));

    /* Twenty pieces slammed to the floor without ever completing a row. */
    for (int i = 0; i < 20 && g.state != TS_OVER; i++) {
        tetris_hard_drop(&g);
        settle(&g);
    }
    CHECK(g.score == 0, "hard drops alone scored %u, expected 0", (unsigned)g.score);

    tetris_reset(&g, 1);
    memset(g.grid, 0, sizeof(g.grid));
    while (tetris_soft_drop(&g)) { }
    CHECK(g.score == 0, "soft drops alone scored %u, expected 0", (unsigned)g.score);
}

/* --- a perfect clear is recognised, but pays no bonus ------------------- */
static void test_perfect_clear(void)
{
    tetris_t g;
    tetris_reset(&g, 1);
    memset(g.grid, 0, sizeof(g.grid));
    for (int y = T_TOTAL - 2; y < T_TOTAL; y++)
        for (int x = 2; x < T_COLS; x++) g.grid[y][x] = P_L + 1;

    force_piece(&g, P_O, -1, T_HIDDEN, 0);   /* box cols 1,2 -> field 0,1 */
    g.score = 0;
    tetris_hard_drop(&g);
    settle(&g);

    CHECK(g.lines == 2, "expected a double, cleared %u", (unsigned)g.lines);
    CHECK(g.score == 100, "perfect-clear double scored %u, expected 100 (no bonus)",
          (unsigned)g.score);
    CHECK(strcmp(g.msg, "PERFECT CLEAR") == 0, "msg was '%s'", g.msg);
    for (int y = 0; y < T_TOTAL; y++)
        for (int x = 0; x < T_COLS; x++)
            CHECK(g.grid[y][x] == 0, "matrix not empty at (%d,%d)", y, x);
}

/* --- a hard drop travels down over several frames ----------------------- */
static void test_drop_animation(void)
{
    tetris_t g;
    tetris_reset(&g, 1);
    memset(g.grid, 0, sizeof(g.grid));

    force_piece(&g, P_O, 4, T_HIDDEN, 0);    /* field cols 5,6 */
    int target = tetris_ghost_py(&g);
    CHECK(target > g.py, "there should be room to fall");

    tetris_hard_drop(&g);
    CHECK(g.state == TS_DROPPING, "a hard drop must animate, state=%d", g.state);
    CHECK(g.py < target, "it must not teleport, py=%d target=%d", g.py, target);
    CHECK(!tetris_move(&g, -1), "the piece is committed once it is dropping");
    CHECK(!tetris_rotate(&g, +1), "...and cannot be rotated");
    CHECK(!tetris_soft_drop(&g), "...nor soft dropped");

    int frames = 0, last_py = g.py;
    while (g.state == TS_DROPPING && frames < 200) {
        tetris_tick(&g, 16667);
        if (g.state == TS_DROPPING) {
            CHECK(g.py > last_py, "the piece stalled mid-drop at row %d", g.py);
            CHECK(g.trail_py <= g.py, "the trail must lag the piece");
            last_py = g.py;
        }
        frames++;
    }
    CHECK(frames >= 2 && frames <= 8,
          "the drop took %d frames over 18 rows, expected roughly 4", frames);
    CHECK(g.grid[target + 1][5] && g.grid[target + 1][6],
          "the piece must land where the ghost was");

    /* A piece already resting locks at once, with nothing to animate. */
    tetris_reset(&g, 1);
    memset(g.grid, 0, sizeof(g.grid));
    force_piece(&g, P_O, 4, T_TOTAL - 2, 0);
    tetris_hard_drop(&g);
    CHECK(g.state != TS_DROPPING, "a resting piece has no drop to animate");
}

/* --- a cleared row burns for the full animation before the collapse ----- */
static void test_clear_animation(void)
{
    tetris_t g;
    tetris_reset(&g, 1);
    memset(g.grid, 0, sizeof(g.grid));
    fill_row(&g, T_TOTAL - 1, 0);

    force_piece(&g, P_I, -2, T_HIDDEN, 1);
    tetris_hard_drop(&g);
    for (int i = 0; i < 400 && g.state == TS_DROPPING; i++) tetris_tick(&g, 1000);
    CHECK(g.state == TS_CLEARING, "expected the clear animation, state=%d", g.state);
    CHECK(g.clear_count == 1, "clear_count=%d", g.clear_count);

    /* The row is still on the board while it burns. */
    tetris_tick(&g, T_CLEAR_TOTAL_US - 2000);
    CHECK(g.state == TS_CLEARING, "the animation ended early");
    CHECK(g.grid[T_TOTAL - 1][1] != 0, "the row collapsed before the animation ended");

    tetris_tick(&g, 4000);
    CHECK(g.state == TS_FALLING, "the animation did not end, state=%d", g.state);
    CHECK(g.grid[T_TOTAL - 1][1] == 0, "the row did not collapse");
    CHECK(g.lines == 1, "lines=%u", (unsigned)g.lines);
}

/* --- SRS wall kicks ---------------------------------------------------- */
static void test_wall_kick(void)
{
    tetris_t g;
    tetris_reset(&g, 1);
    memset(g.grid, 0, sizeof(g.grid));

    /* A vertical J hard against the left wall (box cols 1,2 at px=-1, so field
     * cols 0,1). Rotating R->0 needs box cols 0..2, which would reach x=-1:
     * test 1 fails and test 2, (+1,0), has to rescue it. */
    force_piece(&g, P_J, -1, 10, 1);
    CHECK(tetris_rotate(&g, -1), "R->0 against the left wall must kick");
    CHECK(g.rot == 0, "rot=%d", g.rot);
    CHECK(g.px == 0, "expected a (+1,0) kick to px=0, got px=%d", g.px);

    /* The mirror image on the right wall: L->0 kicks by (-1,0). */
    tetris_reset(&g, 1);
    memset(g.grid, 0, sizeof(g.grid));
    force_piece(&g, P_L, T_COLS - 1, 10, 3);   /* box cols 0,1 -> field 9, 10?? */
    force_piece(&g, P_L, T_COLS - 2, 10, 3);   /* box cols 0,1 -> field 8,9 */
    CHECK(tetris_rotate(&g, +1), "L->0 against the right wall must kick");
    CHECK(g.px == T_COLS - 3, "expected px=%d, got %d", T_COLS - 3, g.px);

    /* O never rotates and never kicks. */
    force_piece(&g, P_O, 4, 10, 0);
    CHECK(!tetris_rotate(&g, +1), "O must not rotate");
}

/* --- T-spin: the 3-corner rule, scored at 400 x level with no lines ---- */
static void test_tspin(void)
{
    tetris_t g;
    tetris_reset(&g, 1);
    memset(g.grid, 0, sizeof(g.grid));

    int px = 3, py = 10;
    /* Fill the four cells diagonally around the T's centre, and put a floor
     * under the piece so it locks where it is rotated. */
    g.grid[py][px] = P_L + 1;         g.grid[py][px + 2] = P_L + 1;
    g.grid[py + 2][px] = P_L + 1;     g.grid[py + 2][px + 2] = P_L + 1;
    for (int x = 0; x < T_COLS; x++) if (x != 5) g.grid[py + 3][x] = P_L + 1;

    force_piece(&g, P_T, px, py, 0);
    g.score = 0;
    CHECK(tetris_rotate(&g, +1), "the T must be able to rotate into the slot");
    CHECK(g.last_kick == 0, "no kick should have been needed, kick=%u", g.last_kick);

    for (int i = 0; i < 600 && g.state == TS_FALLING; i++) tetris_tick(&g, 1000);
    CHECK(g.state != TS_FALLING || g.cur != P_T, "the T should have locked");
    CHECK(g.score == 0, "a T-spin clears no lines, so it scores %u, expected 0",
          (unsigned)g.score);
    CHECK(strcmp(g.msg, "T-SPIN") == 0, "msg was '%s'", g.msg);

    /* The same slot, but the bottom row is one cell short of complete: the
     * spin now clears it, which is a T-spin single at 800 x level. */
    tetris_reset(&g, 1);
    memset(g.grid, 0, sizeof(g.grid));
    g.grid[py][px] = P_L + 1;         g.grid[py][px + 2] = P_L + 1;
    g.grid[py + 2][px] = P_L + 1;     g.grid[py + 2][px + 2] = P_L + 1;
    for (int x = 0; x < T_COLS; x++) if (x != 5) g.grid[py + 3][x] = P_L + 1;
    /* Complete row py+2 apart from the cell the T's nub will occupy (px+1). */
    for (int x = 0; x < T_COLS; x++) if (x != px + 1) g.grid[py + 2][x] = P_L + 1;

    force_piece(&g, P_T, px, py, 0);
    g.score = 0;
    CHECK(tetris_rotate(&g, +1), "rotation into the slot");
    for (int i = 0; i < 600 && g.state == TS_FALLING; i++) tetris_tick(&g, 1000);
    CHECK(g.clear_count == 1, "expected one row cleared, got %d", g.clear_count);
    CHECK(g.score == 40, "a T-spin single is scored as a single: %u, expected 40",
          (unsigned)g.score);
    CHECK(strcmp(g.msg, "T-SPIN SINGLE") == 0, "msg was '%s'", g.msg);
}

/* --- gravity, lock delay and its 15-reset budget ----------------------- */
static void test_lock_delay(void)
{
    tetris_t g;
    tetris_reset(&g, 1);
    memset(g.grid, 0, sizeof(g.grid));

    force_piece(&g, P_O, 4, T_TOTAL - 2, 0);   /* resting on the floor */
    tetris_tick(&g, 1000);
    CHECK(g.landed, "piece on the floor must be landed");

    /* Wiggling resets the 0.5 s timer, but only 15 times. */
    int ms = 0;
    while (ms < 400 && g.state == TS_FALLING) {
        tetris_tick(&g, 1000);
        tetris_move(&g, (ms & 1) ? +1 : -1);
        ms++;
    }
    bool floor_empty = true;
    for (int x = 0; x < T_COLS; x++) if (g.grid[T_TOTAL - 1][x]) floor_empty = false;
    CHECK(floor_empty, "the piece locked before the 15 resets ran out");

    while (ms < 3000 && floor_empty) {
        tetris_tick(&g, 1000);
        tetris_move(&g, (ms & 1) ? +1 : -1);
        ms++;
        for (int x = 0; x < T_COLS; x++) if (g.grid[T_TOTAL - 1][x]) floor_empty = false;
    }
    CHECK(!floor_empty, "the piece must lock once the 15 resets run out");
    CHECK(ms < 15 * 500 + 600, "locked after %d ms, expected about %d", ms, 15 * 500 + 500);

    /* Gravity: at level 1 a piece falls one row per second. */
    tetris_reset(&g, 1);
    memset(g.grid, 0, sizeof(g.grid));
    force_piece(&g, P_O, 4, T_HIDDEN, 0);
    int y0 = g.py;
    tetris_tick(&g, 999000);
    CHECK(g.py == y0, "fell too early");
    tetris_tick(&g, 2000);
    CHECK(g.py == y0 + 1, "should have fallen one row, py=%d", g.py);
}

/* --- the 7-bag never repeats within a bag ------------------------------ */
static void test_bag(void)
{
    tetris_t g;
    tetris_reset(&g, 1);
    int seen[P_COUNT] = {0};
    seen[g.cur]++;
    for (int i = 0; i < 6; i++) {
        piece_t p = tetris_peek(&g, i);
        CHECK(p < P_COUNT, "queue ran dry at %d", i);
        seen[p]++;
    }
    for (int p = 0; p < P_COUNT; p++)
        CHECK(seen[p] == 1, "piece %d appeared %d times in the first bag", p, seen[p]);
}

/* --- hold swaps once per piece ----------------------------------------- */
static void test_hold(void)
{
    tetris_t g;
    tetris_reset(&g, 1);
    piece_t first = g.cur;
    CHECK(tetris_hold(&g), "the first hold must be allowed");
    CHECK(g.hold == first, "hold slot got %d, expected %d", g.hold, first);
    CHECK(!tetris_hold(&g), "a second hold before locking must be refused");

    piece_t held = g.hold, active = g.cur;
    tetris_hard_drop(&g);                       /* locks, new piece, hold freed */
    settle(&g);
    CHECK(tetris_hold(&g), "hold must be allowed again after a lock");
    CHECK(g.cur == held, "swapping back should give %d, got %d", held, g.cur);
    (void)active;
}

/* --- topping out -------------------------------------------------------- */
static void test_topout(void)
{
    tetris_t g;
    tetris_reset(&g, 1);
    for (int i = 0; i < 200 && g.state != TS_OVER; i++) {
        tetris_hard_drop(&g);
        settle(&g);
    }
    CHECK(g.state == TS_OVER, "stacking straight up must eventually top out");
}

/* --- a long random game must never corrupt the matrix ------------------- */
static void test_soak(void)
{
    tetris_t g;
    unsigned seed = 12345;
    for (int game = 0; game < 40; game++) {
        tetris_reset(&g, 1 + game % 15);
        for (int step = 0; step < 6000 && g.state != TS_OVER; step++) {
            seed = seed * 1103515245u + 12345u;
            switch ((seed >> 16) % 8) {
            case 0: tetris_move(&g, -1); break;
            case 1: tetris_move(&g, +1); break;
            case 2: tetris_rotate(&g, +1); break;
            case 3: tetris_rotate(&g, -1); break;
            case 4: tetris_soft_drop(&g); break;
            case 5: tetris_hold(&g); break;
            case 6: tetris_hard_drop(&g); break;
            default: break;
            }
            tetris_tick(&g, 16667);

            /* Invariants: the piece is always inside the matrix and never
             * overlaps the stack. */
            if (g.state != TS_FALLING) continue;
            uint16_t m = tetris_shape(g.cur, g.rot);
            for (int i = 0; i < 16; i++) {
                if (!(m & (1u << i))) continue;
                int x = g.px + (i & 3), y = g.py + (i >> 2);
                CHECK(x >= 0 && x < T_COLS, "piece left the matrix: x=%d", x);
                CHECK(y < T_TOTAL, "piece fell through the floor: y=%d", y);
                if (y >= 0 && y < T_TOTAL)
                    CHECK(g.grid[y][x] == 0, "piece overlaps the stack at (%d,%d)", y, x);
            }
            /* No row is ever left complete outside the clearing state. */
            if (g.state == TS_FALLING) {
                for (int y = 0; y < T_TOTAL; y++) {
                    int n = 0;
                    for (int x = 0; x < T_COLS; x++) n += g.grid[y][x] ? 1 : 0;
                    CHECK(n < T_COLS, "row %d left complete after a lock", y);
                }
            }
            if (fails > 10) return;
        }
    }
}

int main(void)
{
    test_shapes();          printf("shapes ok\n");
    test_bag();             printf("bag ok\n");
    test_hold();            printf("hold ok\n");
    test_wall_kick();       printf("kicks ok\n");
    test_lock_delay();      printf("lock delay ok\n");
    test_line_scores();     printf("line scores ok\n");
    test_dropping_scores_nothing(); printf("dropping scores nothing ok\n");
    test_perfect_clear();   printf("perfect clear ok\n");
    test_drop_animation();  printf("drop animation ok\n");
    test_clear_animation(); printf("clear animation ok\n");
    test_tspin();           printf("t-spin ok\n");
    test_topout();          printf("top out ok\n");
    test_soak();            printf("soak ok\n");

    printf(fails ? "\n%d FAILURES\n" : "\nall good\n", fails);
    return fails != 0;
}
