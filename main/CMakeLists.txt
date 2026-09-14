idf_component_register(
    SRCS
        "app_main.cpp"
        "passport_audio.cpp"
        "passport_board.cpp"
        "network_config.cpp"
        "network_uploader.cpp"
        "recording_store.cpp"
        "recorder_ui.cpp"
        "web_export.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_i2s
        esp_driver_spi
        esp_event
        esp_http_server
        esp_http_client
        esp_lcd
        esp_netif
        esp-tls
        esp_wifi
        fatfs
        nvs_flash
        wear_levelling
        espressif__button
        espressif__cmake_utilities
        espressif__esp_codec_dev
        espressif__esp_audio_codec
        espressif__esp_lvgl_port
        lvgl__lvgl
        mbedtls
)

target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)
target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
