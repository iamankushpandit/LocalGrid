idf_component_register(SRCS "node_main.c"
                            "backbone.c"
                            "sessions.c"
                            "ble_adv.c"
                            "console_cmds.c"
                            "settings.c"
                            "web_admin.c"
                            "grid_state.c"
                            "power_trace.c"
                       INCLUDE_DIRS "." "../../common"
                       EMBED_TXTFILES "web/admin.html"
                       EMBED_FILES "../../../assets/brand/localgrid-icon.svg"
                       REQUIRES lg_core lg_crypto lg_identity lg_power esp_wifi esp_netif esp_event nvs_flash
                                bt console esp_timer lwip esp_http_server)
