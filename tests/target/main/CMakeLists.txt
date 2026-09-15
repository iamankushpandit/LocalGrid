idf_component_register(SRCS "test_main.c"
                            "test_core.c"
                            "test_crypto.c"
                            "test_messaging.c"
                            "test_screen.c"
                            "sim.c"
                       INCLUDE_DIRS "."
                       REQUIRES lg_core lg_crypto lg_identity lg_board lg_ui esp_timer nvs_flash)
