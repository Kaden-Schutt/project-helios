idf_component_register(
    SRCS "main.c" "camera.c" "wifi.c" "ota.c" "diag_log.c"
         "button.c" "mic_probe.c" "mic.c"
         "ota_verify.c" "sd_card.c" "recovery.c"
    INCLUDE_DIRS "." "../include"
    REQUIRES driver esp_timer esp_system nvs_flash
             esp_wifi esp_event esp_netif esp_http_server app_update
             esp32-camera mdns mbedtls
             fatfs sdmmc vfs
)
