idf_component_register(
    SRCS "cn105.cpp" "cn105_codec.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_uart esp_timer
)
