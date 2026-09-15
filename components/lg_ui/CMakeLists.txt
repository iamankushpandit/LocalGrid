# lg_ui: display and touch bring-up, LVGL integration, and the theme (decisions D9, D10, D23).
idf_component_register(SRCS src/lg_display.c
                            src/lg_theme.c
                            src/lg_touch.c
                            src/lg_calibrate.c
                       INCLUDE_DIRS include
                       REQUIRES lvgl__lvgl lg_board
                       PRIV_REQUIRES esp_lcd esp_driver_spi esp_driver_gpio esp_driver_i2c esp_timer nvs_flash)
