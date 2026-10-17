# Pure BTHome parser, Shelly Wi-Fi H&T helpers + pairing/freshness tracker (host-tested, no IDF deps).
idf_component_register(
    SRCS "bthome.cpp" "sensor_set.cpp" "shelly_ht.cpp"
    INCLUDE_DIRS "include"
)
