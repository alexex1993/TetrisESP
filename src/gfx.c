#include "gfx.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_lcd_jd9853.h"
#include "font5x7.h"

static const char *TAG = "gfx";

static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_tile;                 /* DMA staging buffer, GFX_W*GFX_TILE_ROWS px */
static SemaphoreHandle_t s_flush_done;   /* given when the DMA is done with s_tile */

/* ------------------------------------------------------------------ */
/*  Primitives                                                         */
/* ------------------------------------------------------------------ */

uint16_t gfx_rgb(int r, int g, int b)
{
    if (r < 0) { r = 0; } if (r > 255) { r = 255; }
    if (g < 0) { g = 0; } if (g > 255) { g = 255; }
    if (b < 0) { b = 0; } if (b > 255) { b = 255; }
    return COLOR(r, g, b);
}

void gfx_pixel(canvas_t *c, int x, int y, uint16_t color)
{
    int lx = x - c->x0;
    int ly = y - c->y0;
    if (lx < 0 || ly < 0 || lx >= c->w || ly >= c->h) {
        return;
    }
    c->buf[ly * c->w + lx] = color;
}

/* Clipped up front rather than per pixel: painters repaint their whole
 * rectangle for every band, so most calls are entirely off-band. */
void gfx_fill(canvas_t *c, int x, int y, int w, int h, uint16_t color)
{
    int lx0 = x - c->x0, ly0 = y - c->y0;
    int lx1 = lx0 + w,   ly1 = ly0 + h;

    if (lx0 < 0) { lx0 = 0; }
    if (ly0 < 0) { ly0 = 0; }
    if (lx1 > c->w) { lx1 = c->w; }
    if (ly1 > c->h) { ly1 = c->h; }

    for (int ly = ly0; ly < ly1; ly++) {
        uint16_t *row = &c->buf[ly * c->w];
        for (int lx = lx0; lx < lx1; lx++) {
            row[lx] = color;
        }
    }
}

void gfx_frame(canvas_t *c, int x, int y, int w, int h, int t, uint16_t color)
{
    gfx_fill(c, x, y, w, t, color);
    gfx_fill(c, x, y + h - t, w, t, color);
    gfx_fill(c, x, y + t, t, h - 2 * t, color);
    gfx_fill(c, x + w - t, y + t, t, h - 2 * t, color);
}

void gfx_char(canvas_t *c, int x, int y, char ch, int scale, uint16_t color)
{
    if (x + FONT5X7_WIDTH * scale <= c->x0 || x >= c->x0 + c->w ||
        y + FONT5X7_HEIGHT * scale <= c->y0 || y >= c->y0 + c->h) {
        return;   /* glyph is entirely outside this band */
    }
    if (ch < FONT5X7_FIRST_CHAR || ch > FONT5X7_LAST_CHAR) {
        ch = '?';
    }
    const uint8_t *glyph = &font5x7[(ch - FONT5X7_FIRST_CHAR) * FONT5X7_WIDTH];

    for (int col = 0; col < FONT5X7_WIDTH; col++) {
        uint8_t bits = glyph[col];
        for (int row = 0; row < FONT5X7_HEIGHT; row++) {
            if (!(bits & (1 << row))) {
                continue;
            }
            if (scale == 1) {
                gfx_pixel(c, x + col, y + row, color);
            } else {
                gfx_fill(c, x + col * scale, y + row * scale, scale, scale, color);
            }
        }
    }
}

int gfx_text_w(const char *s, int scale)
{
    int n = (int)strlen(s);
    return n ? (n * (FONT5X7_WIDTH + 1) - 1) * scale : 0;
}

void gfx_text(canvas_t *c, int x, int y, const char *s, int scale, uint16_t color)
{
    for (; *s; s++) {
        gfx_char(c, x, y, *s, scale, color);
        x += (FONT5X7_WIDTH + 1) * scale;
    }
}

void gfx_text_center(canvas_t *c, int y, const char *s, int scale, uint16_t color)
{
    gfx_text(c, (GFX_W - gfx_text_w(s, scale)) / 2, y, s, scale, color);
}

/* ------------------------------------------------------------------ */
/*  Flushing                                                           */
/* ------------------------------------------------------------------ */

void gfx_region(int x, int y, int w, int h, gfx_painter_t painter, void *ctx)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > GFX_W) { w = GFX_W - x; }
    if (y + h > GFX_H) { h = GFX_H - y; }
    if (w <= 0 || h <= 0) {
        return;
    }

    int max_rows = (GFX_W * GFX_TILE_ROWS) / w;
    if (max_rows < 1) {
        max_rows = 1;
    }

    for (int by = y; by < y + h; by += max_rows) {
        int bh = (by + max_rows > y + h) ? (y + h - by) : max_rows;
        canvas_t c = { .x0 = x, .y0 = by, .w = w, .h = bh, .buf = s_tile };
        painter(&c, ctx);
        ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(s_panel, x, by, x + w, by + bh, s_tile));
        /* draw_bitmap only queues the DMA transfer: the next band must not
         * paint over s_tile until the panel has consumed this one. */
        xSemaphoreTake(s_flush_done, portMAX_DELAY);
    }
}

/* ------------------------------------------------------------------ */
/*  Bring-up                                                           */
/* ------------------------------------------------------------------ */

/* GPIO23 drives an NPN base through 1k, and carries the SoC's weak pull-up at
 * reset -- enough to switch that transistor on. So the pin is claimed at 0 %
 * duty before the panel is touched, and only raised after the first flush. */
static void backlight_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));

    ledc_channel_config_t channel = {
        .gpio_num = BSP_LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&channel));
}

void gfx_backlight(uint8_t percent)
{
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, percent * 255 / 100));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0));
}

static bool IRAM_ATTR on_color_trans_done(esp_lcd_panel_io_handle_t io,
                                          esp_lcd_panel_io_event_data_t *edata,
                                          void *user_ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_flush_done, &woken);
    return woken == pdTRUE;
}

void gfx_init(void)
{
    backlight_init();

    /* The TF card shares SCLK/MOSI with the panel and is separated only by
     * chip select: an unparked card corrupts the display's traffic. */
    gpio_config_t sd_cs = { .pin_bit_mask = 1ULL << BSP_SD_CS, .mode = GPIO_MODE_OUTPUT };
    ESP_ERROR_CHECK(gpio_config(&sd_cs));
    gpio_set_level(BSP_SD_CS, 1);

    spi_bus_config_t bus = {
        .sclk_io_num = BSP_LCD_SCLK,
        .mosi_io_num = BSP_LCD_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = GFX_W * GFX_TILE_ROWS * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(BSP_LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO));

    s_flush_done = xSemaphoreCreateBinary();
    ESP_ERROR_CHECK(s_flush_done ? ESP_OK : ESP_ERR_NO_MEM);

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = BSP_LCD_CS,
        .dc_gpio_num = BSP_LCD_DC,
        .spi_mode = 0,
        .pclk_hz = BSP_LCD_PIXEL_CLK,
        .trans_queue_depth = 1,
        .on_color_trans_done = on_color_trans_done,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BSP_LCD_SPI_HOST,
                                             &io_cfg, &io));

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = BSP_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_jd9853(io, &panel_cfg, &s_panel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    /* The visible 172 columns start at column 34 of the controller's RAM. */
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(s_panel, JD9853_LCD_X_GAP, JD9853_LCD_Y_GAP));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    s_tile = heap_caps_malloc(GFX_W * GFX_TILE_ROWS * sizeof(uint16_t), MALLOC_CAP_DMA);
    ESP_ERROR_CHECK(s_tile ? ESP_OK : ESP_ERR_NO_MEM);

    ESP_LOGI(TAG, "panel ready: %dx%d, %d MHz, %d B staging buffer",
             GFX_W, GFX_H, BSP_LCD_PIXEL_CLK / 1000000,
             (int)(GFX_W * GFX_TILE_ROWS * sizeof(uint16_t)));
}
