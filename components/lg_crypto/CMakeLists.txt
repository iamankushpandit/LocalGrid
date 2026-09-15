# lg_crypto: LocalGrid cryptography adapter over PSA Crypto (Mbed TLS 4, ESP-IDF v6.x).
# Protocol code never calls PSA or mbedtls directly; everything goes through lg_crypto.h.
idf_component_register(SRCS src/lg_crypto.c
                       INCLUDE_DIRS include
                       REQUIRES mbedtls)
