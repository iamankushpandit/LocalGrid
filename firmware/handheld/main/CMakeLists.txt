# service/ owns Wi-Fi, the node session, and lg_client; ui/ owns screens. They meet only in hh_service.h (D27).
# voice/ owns the microphone and speaker for push-to-talk (D61) and reaches the network through hh_service.h.
idf_component_register(SRCS "hh_main.c"
                            "../../common/lg_timekeep.c"
                            "hh_console.c"
                            "hh_mem.c"
                            "service/hh_service.c"
                            "service/hh_adpcm.c"
                            "service/hh_battery.c"
                            "ui/ui_main.c"
                            "ui/ui_lock.c"
                            "ui/ui_chat.c"
                            "ui/ui_kb.c"
                            "ui/ui_list.c"
                            "ui/ui_screens.c"
                            "ui/ui_overlay.c"
                            "voice/hh_voice.c"
                       INCLUDE_DIRS "." "service" "ui" "voice" "../../common"
                       REQUIRES lg_draw lg_core lg_crypto lg_identity lg_board lg_bsp lg_selftest lg_power
                                esp_wifi esp_netif esp_event nvs_flash lwip esp_timer console)
