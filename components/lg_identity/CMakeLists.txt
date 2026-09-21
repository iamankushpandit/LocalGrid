# lg_identity: reads the LocalGrid device identity from the "lgid" partition, checks it was minted
# for this board, and answers "id" on serial. lg_crypto supplies the SHA-256 the check recomputes with.
idf_component_register(SRCS src/lg_identity.c
                       INCLUDE_DIRS include
                       REQUIRES nvs_flash
                       PRIV_REQUIRES lg_crypto esp_hw_support esp_driver_uart esp_driver_usb_serial_jtag vfs)
