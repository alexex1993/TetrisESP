idf_component_register(SRCS "main.c" "gfx.c" "tetris.c" "render.c" "input.c"
                       INCLUDE_DIRS "." "../include"
                       REQUIRES jd9853 axs5106l esp_lcd nvs_flash esp_timer
                                esp_driver_spi esp_driver_i2c
                                esp_driver_ledc esp_driver_gpio)
