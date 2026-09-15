idf_component_register(SRCS "test_main.c"
                            "test_core.c"
                            "test_crypto.c"
                            "test_messaging.c"
                            "sim.c"
                       INCLUDE_DIRS "."
                       REQUIRES lg_core lg_crypto)
