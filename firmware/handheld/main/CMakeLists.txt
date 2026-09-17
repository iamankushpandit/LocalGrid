# service/ owns Wi-Fi, the node session, and lg_client; ui/ owns screens. They meet only in hh_service.h (D27).
idf_component_register(SRCS "hh_main.c"
                            "hh_console.c"
                            "hh_mem.c"
                            "service/hh_service.c"
                            "ui/ui_bar.c"
                            "ui/ui_list.c"
                            "ui/ui_snapshot.c"
                            "ui/ui_screen.c"
                            "ui/ui_launcher.c"
                            "ui/ui_settings.c"
                            "ui/ui_home.c"
                            "ui/ui_chat.c"
                            "ui/ui_group.c"
                            "ui/ui_notify.c"
                            "ui/ui_alert.c"
                       INCLUDE_DIRS "." "service" "ui" "../../common"
                       REQUIRES lg_core lg_crypto lg_identity lg_board lg_bsp lg_ui lg_selftest lg_power
                                esp_wifi esp_netif esp_event nvs_flash lwip esp_timer console)
