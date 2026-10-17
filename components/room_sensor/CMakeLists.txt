# Pure BTHome parser + pairing/freshness tracker (host-tested, no IDF deps).
idf_component_register(
    SRCS "bthome.cpp" "sensor_tracker.cpp"
    INCLUDE_DIRS "include"
)
