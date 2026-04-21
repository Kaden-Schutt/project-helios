idf_component_register(
    SRCS "main.c" "camera.c" "mic.c"
    INCLUDE_DIRS "." "../include"
    REQUIRES driver esp_timer esp_system nvs_flash esp32-camera
)
