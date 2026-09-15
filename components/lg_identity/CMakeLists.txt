# lg_identity: reads the LocalGrid device identity from the "lgid" partition and answers "id" on serial.
idf_component_register(SRCS src/lg_identity.c
                       INCLUDE_DIRS include
                       REQUIRES nvs_flash
                       PRIV_REQUIRES esp_driver_uart esp_driver_usb_serial_jtag vfs)
