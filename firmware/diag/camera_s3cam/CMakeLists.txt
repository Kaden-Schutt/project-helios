idf_component_register(
    SRCS "main.c" "camera.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_timer esp_system nvs_flash esp32-camera
)
