idf_component_register(
    SRCS "hvac_mqtt.cpp" "hvac_topics.cpp"
    INCLUDE_DIRS "include"
    REQUIRES cn105 nvs_flash
)
