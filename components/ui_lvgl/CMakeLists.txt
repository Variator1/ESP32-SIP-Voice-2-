idf_component_register(SRCS "ui_lvgl.c"
                       INCLUDE_DIRS "."
                       REQUIRES board_hal esp_timer config_store)