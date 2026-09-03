#include "input.h"

#include <stdlib.h>

#include "esp_log.h"

#include "gfx.h"
#include "render.h"

static const char *TAG = "input";

#define STEP_PX          12      /* travel that moves the piece one cell */
#define TAP_MAX_DEV      10      /* a tap may wander this far and stay a tap */
#define TAP_MAX_US       320000
#define FLICK_SPEED_PX   20      /* per sample: a deliberate flick, not a drag */
#define FLICK_MIN_PX     24      /* ...and it must have covered this much ground */
#define FLICK_TOTAL_PX   46
#define FLICK_MAX_US     260000

static axs5106l_handle_t s_tp;

static struct {
    bool     down;
    int64_t  t_down;
    int      x0, y0;      /* where the contact started */
    int      ax, ay;      /* anchors, advanced one STEP_PX at a time */
    int      lx, ly;      /* previous sample */
    int      dev;         /* furthest the contact has been from its start */
    bool     dragged;     /* a step was emitted, so this is not a tap */
    bool     flicked;     /* a hard drop or hold already fired for this contact */
} s;

void input_init(axs5106l_handle_t tp)
{
    s_tp = tp;
    input_reset();
}

void input_reset(void)
{
    s.down = false;
    s.dragged = false;
    s.flicked = false;
}

bool input_touching(void)
{
    return s.down;
}

static void push(input_events_t *o, input_ev_t e)
{
    if (o->n < IN_MAX_EVENTS) {
        o->ev[o->n++] = e;
    }
}

void input_poll(input_events_t *out, int64_t now_us)
{
    out->n = 0;

    axs5106l_data_t t;
    if (axs5106l_read(s_tp, &t) != ESP_OK) {
        return;      /* a dropped I2C frame is not a release */
    }

    if (!t.pressed) {
        if (s.down) {
            bool tap = !s.dragged && !s.flicked &&
                       s.dev <= TAP_MAX_DEV && (now_us - s.t_down) <= TAP_MAX_US;
            if (tap) {
                push(out, IN_ANY_TAP);
                if (s.y0 < HUD_H) {
                    push(out, IN_PAUSE);
                } else {
                    push(out, s.x0 < GFX_W / 2 ? IN_ROT_CCW : IN_ROT_CW);
                }
            }
        }
        s.down = false;
        return;
    }

    int x = t.x, y = t.y;

    if (!s.down) {
        s.down = true;
        s.t_down = now_us;
        s.x0 = s.ax = s.lx = x;
        s.y0 = s.ay = s.ly = y;
        s.dev = 0;
        s.dragged = false;
        s.flicked = false;
        return;
    }

    int fdx = x - s.lx, fdy = y - s.ly;   /* this sample */
    s.lx = x;
    s.ly = y;

    int tdx = x - s.x0, tdy = y - s.y0;   /* since the contact started */
    int dev = abs(tdx) > abs(tdy) ? abs(tdx) : abs(tdy);
    if (dev > s.dev) {
        s.dev = dev;
    }

    /* Flicks first: once one fires, the rest of the contact is dead, so a
     * hard drop cannot also drag the next piece sideways. */
    if (!s.flicked && abs(fdy) > 2 * abs(fdx)) {
        /* One fast sample is the signal, but it still has to have travelled:
         * a single noisy jump must not read as a hard drop. */
        bool fast = abs(fdy) >= FLICK_SPEED_PX && abs(tdy) >= FLICK_MIN_PX;
        bool far_enough = abs(tdy) >= FLICK_TOTAL_PX &&
                          (now_us - s.t_down) <= FLICK_MAX_US;
        if (fast || far_enough) {
            int dir = fast ? fdy : tdy;
            s.flicked = true;
            s.dragged = true;
            push(out, dir > 0 ? IN_HARD_DROP : IN_HOLD);
            return;
        }
    }
    if (s.flicked) {
        return;
    }

    while (x - s.ax >= STEP_PX)  { s.ax += STEP_PX; push(out, IN_RIGHT); s.dragged = true; }
    while (s.ax - x >= STEP_PX)  { s.ax -= STEP_PX; push(out, IN_LEFT);  s.dragged = true; }

    while (y - s.ay >= STEP_PX)  { s.ay += STEP_PX; push(out, IN_SOFT_DROP); s.dragged = true; }
    if (s.ay - y >= STEP_PX) {
        s.ay = y;    /* pieces never go up: just follow the finger back */
    }

    if (out->n >= IN_MAX_EVENTS) {
        ESP_LOGD(TAG, "gesture queue full");
    }
}
