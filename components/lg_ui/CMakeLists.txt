# lg_ui: LVGL integration, theme, widgets, pointer input, and calibration screen (decisions D9, D10, D23, D27).
# Reaches hardware only through lg_bsp; never includes ESP-IDF drivers.
idf_component_register(SRCS src/lg_display.c
                            src/lg_theme.c
                            src/lg_ui_input.c
                            src/lg_ui_calibrate.c
                            src/lg_ui_widgets.c
                       INCLUDE_DIRS include
                       REQUIRES lvgl__lvgl lg_board lg_bsp
                       PRIV_REQUIRES esp_timer)
