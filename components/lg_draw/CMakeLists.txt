# lg_draw: the handheld renderer and its fonts, with no UI library (D55). Reaches the panel and
# touch only through lg_bsp (D27).
idf_component_register(SRCS src/lg_draw.c
                            src/lg_draw_calibrate.c
                            fonts/lg_font_montserrat_10.c
                            fonts/lg_font_montserrat_12.c
                            fonts/lg_font_montserrat_14.c
                            fonts/lg_font_montserrat_16.c
                            fonts/lg_font_montserrat_20.c
                            fonts/lg_font_montserrat_28.c
                            fonts/lg_font_emoji_14.c
                            fonts/lg_font_emoji_20.c
                       INCLUDE_DIRS include
                       REQUIRES lg_board lg_bsp
                       PRIV_REQUIRES esp_timer)
