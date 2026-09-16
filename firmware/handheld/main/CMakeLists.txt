# service/ owns Wi-Fi, the node session, and lg_client; ui/ owns screens. They meet only in hh_service.h (D27).
idf_component_register(SRCS "hh_main.c"
                            "hh_console.c"
                            "service/hh_service.c"
                            "ui/ui_home.c"
                            "ui/ui_chat.c"
                            "ui/ui_notify.c"
                       INCLUDE_DIRS "." "service" "ui" "../../common"
                       REQUIRES lg_core lg_crypto lg_identity lg_board lg_bsp lg_ui
                                esp_wifi esp_netif esp_event nvs_flash lwip esp_timer console)
