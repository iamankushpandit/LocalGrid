idf_component_register(SRCS "node_main.c"
                            "../../common/lg_timekeep.c"
                            "backbone.c"
                            "sessions.c"
                            "ble_adv.c"
                            "ble_status.c"
                            "ble_link.c"
                            "traffic.c"
                            "console_cmds.c"
                            "settings.c"
                            "web_admin.c"
                            "grid_state.c"
                            "tz_names.c"
                            "power_trace.c"
                            "chaos_self.c"
                            "chaos_log.c"
                            "gps.c"
                            "lora.c"
                            "../../common/lg_lora_wire.c"
                            "../../common/lg_lora_hh.c"
                            "../../common/lg_gridkeys.c"
                            "../../common/lg_fwupdate.c"
                       INCLUDE_DIRS "." "../../common"
                       EMBED_TXTFILES "web/admin.html"
                       EMBED_FILES "../../../assets/brand/localgrid-icon.svg"
                       REQUIRES lg_core lg_crypto lg_identity lg_power esp_wifi esp_netif esp_event nvs_flash
                                bt console esp_timer lwip esp_http_server app_update esp_partition esp_driver_uart esp_driver_gpio)

# The access point is the one firmware that holds the backbone key (D96): a handheld image has no
# copy of it to fall back on, even on the bench.
target_compile_definitions(${COMPONENT_LIB} PRIVATE LG_GRIDKEYS_WITH_BACKBONE=1)
