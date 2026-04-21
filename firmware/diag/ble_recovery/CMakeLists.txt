idf_component_register(
    SRCS "main.c" "ota_verify.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_system app_update bt mbedtls
)
