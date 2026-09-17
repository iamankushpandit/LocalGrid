# lg_bsp: board support drivers (decision D27): SPI and I2C buses, display panel, touch controllers.
# Knows board profiles; knows nothing about screens or the network.
idf_component_register(SRCS src/lg_bsp_display.c
                            src/lg_bsp_touch.c
                            src/lg_bsp_audio.c
                            src/lg_bsp_settings.c
                       INCLUDE_DIRS include
                       REQUIRES lg_board
                       PRIV_REQUIRES esp_lcd esp_driver_spi esp_driver_gpio esp_driver_i2c esp_driver_dac esp_driver_i2s esp_timer nvs_flash)
