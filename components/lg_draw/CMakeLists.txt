# lg_draw: a retained-box renderer with no UI library (spike). Reaches the panel and touch only
# through lg_bsp, like lg_ui (D27). LVGL is listed only for its font table types.
idf_component_register(SRCS src/lg_draw.c
                       INCLUDE_DIRS include
                       REQUIRES lg_board lg_bsp lvgl__lvgl
                       PRIV_REQUIRES esp_timer)
