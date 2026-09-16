# lg_selftest: the quick check every device runs at boot (decision D24).
# Pure protocol and crypto checks with no simulated grid, so it fits beside Wi-Fi.
idf_component_register(SRCS src/lg_selftest.c
                       INCLUDE_DIRS include
                       REQUIRES lg_core lg_crypto
                       PRIV_REQUIRES esp_timer)
