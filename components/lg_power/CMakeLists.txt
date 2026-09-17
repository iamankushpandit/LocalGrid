# lg_power: supply voltage from a board's sense divider, and the `power` console command.
# Infrastructure-safe: no UI, no LVGL. Used by AP and handheld firmware alike.
idf_component_register(SRCS src/lg_power.c
                       INCLUDE_DIRS include
                       PRIV_REQUIRES esp_adc esp_timer console)
