/*
 * Tetris for the Waveshare ESP32-C6-Touch-LCD-1.47.
 *
 * 10x20 guideline matrix on the 172x320 JD9853 panel, driven entirely from
 * the AXS5106L capacitive touch panel, running a fixed 60 Hz frame loop.
 *
 * How 60 fps is possible on a bus that caps full-screen flushes at ~45 fps:
 * nothing ever redraws the whole screen during play. The renderer keeps a
 * 10x20 shadow of what the panel is showing and pushes only the cells that
 * changed -- a moving piece is 8 to 16 cells, about 1.5 ms of SPI traffic
 * against a 16.7 ms budget. See render.c.
 *
 *   src/tetris.c  rules: SRS, wall kicks, 7-bag, lock delay, scoring
 *   src/render.c  the matrix, the HUD and the overlays
 *   src/input.c   touch gestures
 *   src/gfx.c     panel bring-up and the tile renderer
 */

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "axs5106l.h"
#include "board_pins.h"
#include "gfx.h"
#include "input.h"
#include "render.h"
#include "tetris.h"

static const char *TAG = "tetris";

#define FRAME_US        16667          /* 60 fps */
#define DT_CLAMP_US     50000          /* never advance the game by more than this */
#define OVER_LOCKOUT_US 700000         /* ignore taps right after a top-out */
#define START_LEVEL     1

#define NVS_NS   "tetris"
#define NVS_BEST "best"

typedef enum { SC_TITLE, SC_PLAY, SC_PAUSE, SC_OVER } screen_t;

static tetris_t s_game;
static uint32_t s_hiscore;

/* ------------------------------------------------------------------ */

static void nvs_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

static uint32_t hiscore_load(void)
{
    nvs_handle_t h;
    uint32_t v = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u32(h, NVS_BEST, &v);
        nvs_close(h);
    }
    return v;
}

static void hiscore_save(uint32_t v)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "cannot open NVS, the high score will not survive a reset");
        return;
    }
    if (nvs_set_u32(h, NVS_BEST, v) == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
}

static i2c_master_bus_handle_t i2c_init(void)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port = BSP_I2C_PORT,
        .sda_io_num = BSP_I2C_SDA,
        .scl_io_num = BSP_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus = NULL;
    ESP_ERROR_CHECK(i2c_new_master_bus(&cfg, &bus));
    return bus;
}

static axs5106l_handle_t touch_init(i2c_master_bus_handle_t bus)
{
    /* Rotation 0 on this panel is "mirror X, leave Y alone" -- the mapping the
     * skill's template verified on hardware, so no calibration flow is needed. */
    axs5106l_config_t cfg = {
        .i2c_bus = bus,
        .rst_gpio = BSP_TP_RST,
        .int_gpio = BSP_TP_INT,
        .width = GFX_W,
        .height = GFX_H,
        .mirror_x = true,
    };
    axs5106l_handle_t tp = NULL;
    esp_err_t err = axs5106l_new(&cfg, &tp);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "touch controller did not answer: %s", esp_err_to_name(err));
        return NULL;
    }

    uint16_t fw = 0;
    uint32_t id = 0;
    if (axs5106l_read_info(tp, &fw, &id) == ESP_OK) {
        ESP_LOGI(TAG, "AXS5106L ready: chip ID 0x%06X, firmware V%u",
                 (unsigned)id, (unsigned)fw);
    }
    return tp;
}

/* ------------------------------------------------------------------ */

static void apply_events(const input_events_t *in)
{
    for (int i = 0; i < in->n; i++) {
        switch (in->ev[i]) {
        case IN_LEFT:       tetris_move(&s_game, -1);   break;
        case IN_RIGHT:      tetris_move(&s_game, +1);   break;
        case IN_ROT_CW:     tetris_rotate(&s_game, +1); break;
        case IN_ROT_CCW:    tetris_rotate(&s_game, -1); break;
        case IN_SOFT_DROP:  tetris_soft_drop(&s_game);  break;
        case IN_HARD_DROP:  tetris_hard_drop(&s_game);  break;
        case IN_HOLD:       tetris_hold(&s_game);       break;
        default: break;
        }
    }
}

static bool has_event(const input_events_t *in, input_ev_t e)
{
    for (int i = 0; i < in->n; i++) {
        if (in->ev[i] == e) {
            return true;
        }
    }
    return false;
}

void app_main(void)
{
    ESP_LOGI(TAG, "Tetris on ESP32-C6-Touch-LCD-1.47");

    nvs_init();
    gfx_init();
    render_init();

    s_hiscore = hiscore_load();
    render_title(s_hiscore);
    gfx_backlight(90);            /* only after the first full flush */

    axs5106l_handle_t tp = touch_init(i2c_init());
    if (tp == NULL) {
        ESP_LOGE(TAG, "no touch controller: the game cannot be played");
        vTaskDelay(portMAX_DELAY);
    }
    input_init(tp);

    screen_t screen = SC_TITLE;
    int64_t over_at = 0;

    int fps = 60, frames = 0;
    int64_t fps_window = esp_timer_get_time();
    int64_t work_sum = 0, work_max = 0;

    int64_t next = esp_timer_get_time();
    int64_t last = next;

    while (true) {
        int64_t now = esp_timer_get_time();
        int64_t dt = now - last;
        last = now;
        if (dt > DT_CLAMP_US) {
            dt = DT_CLAMP_US;
        }

        input_events_t in;
        input_poll(&in, now);

        switch (screen) {
        case SC_TITLE:
            if (has_event(&in, IN_ANY_TAP)) {
                tetris_reset(&s_game, START_LEVEL);
                render_full(&s_game, fps);
                input_reset();
                screen = SC_PLAY;
            }
            break;

        case SC_PLAY:
            if (has_event(&in, IN_PAUSE)) {
                render_pause();
                input_reset();
                screen = SC_PAUSE;
                break;
            }
            apply_events(&in);
            tetris_tick(&s_game, dt);
            render_frame(&s_game, fps);

            if (s_game.state == TS_OVER) {
                bool record = s_game.score > s_hiscore;
                if (record) {
                    s_hiscore = s_game.score;
                    hiscore_save(s_hiscore);
                }
                ESP_LOGI(TAG, "game over: score %lu, %lu lines, level %u%s",
                         (unsigned long)s_game.score, (unsigned long)s_game.lines,
                         (unsigned)s_game.level, record ? "  (record)" : "");
                render_gameover(&s_game, s_hiscore, record);
                input_reset();
                over_at = now;
                screen = SC_OVER;
            }
            break;

        case SC_PAUSE:
            if (has_event(&in, IN_ANY_TAP)) {
                render_full(&s_game, fps);
                input_reset();
                screen = SC_PLAY;
            }
            break;

        case SC_OVER:
            if (has_event(&in, IN_ANY_TAP) && now - over_at > OVER_LOCKOUT_US) {
                render_title(s_hiscore);
                input_reset();
                screen = SC_TITLE;
            }
            break;
        }

        /* Frame accounting: work time is everything above, the rest is slack. */
        int64_t worked = esp_timer_get_time() - now;
        work_sum += worked;
        if (worked > work_max) {
            work_max = worked;
        }
        frames++;
        if (now - fps_window >= 1000000) {
            fps = (int)((int64_t)frames * 1000000 / (now - fps_window));
            ESP_LOGI(TAG, "%d fps, frame work avg %lld us, max %lld us",
                     fps, work_sum / frames, work_max);
            frames = 0;
            work_sum = 0;
            work_max = 0;
            fps_window = now;
        }

        next += FRAME_US;
        int64_t remain = next - esp_timer_get_time();
        if (remain < -4 * FRAME_US) {
            next = esp_timer_get_time();   /* resync after a long stall */
            remain = 0;
        }
        vTaskDelay(remain >= 1000 ? pdMS_TO_TICKS(remain / 1000) : 1);
    }
}
