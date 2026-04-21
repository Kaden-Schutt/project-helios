idf_component_register(
    SRCS "main.c" "speaker_pcm.c" "wifi.c" "ota.c" "diag_log.c"
    INCLUDE_DIRS "." "../include"
    REQUIRES driver esp_timer esp_system nvs_flash esp_wifi esp_event esp_netif esp_http_server app_update mdns
)
