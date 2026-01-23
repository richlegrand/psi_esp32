# Base sources - always compile both camera and JPEG support
# Actual mode auto-detected at runtime (checks /littlefs/frames directory)
set(SRCS
    "psi_main.cpp"
    "httpd_server.cpp"
    "httpd_test.c"
    "video_streamer.cpp"
    "jpeg_decoder.cpp"           # JPEG decoder (used if JPEG files detected)
    "jpeg_frame_reader.cpp"      # JPEG frame reader (used if JPEG files detected)
)

# All requirements (both camera and JPEG support)
set(COMPONENT_REQUIRES
    libdatachannel
    esp_netif
    esp_event
    nvs_flash        # Required for WiFi configuration storage
    esp_wifi         # WiFi connectivity
    esp_wifi_remote  # WiFi Remote for ESP32-C6 co-processor
    esp_hosted       # ESP-Hosted transport for ESP32-C6
    littlefs         # LittleFS for media file storage
    json             # Built-in cJSON for WebSocket signaling
    esp_websocket_client  # WebSocket client for signaling
    esp_http_server  # For httpd_uri_t and httpd_req_t types
    esp_video        # ESP32-P4 video capture and H.264 encoding
    esp_driver_ppa   # Pixel Processing Accelerator for hardware scaling
    esp_driver_jpeg  # Hardware JPEG decoder
    example_video_common  # Board-specific video initialization (camera mode)
)

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS "."
    REQUIRES ${COMPONENT_REQUIRES}
)

# Create LittleFS image from media_files directory
# This will package all H.264 files into the 'storage' partition at build time
littlefs_create_partition_image(
    storage                                      # partition name (must match partitions.csv)
    ${CMAKE_CURRENT_SOURCE_DIR}/../media_files  # source directory
    FLASH_IN_PROJECT                            # flash when running idf.py flash
)