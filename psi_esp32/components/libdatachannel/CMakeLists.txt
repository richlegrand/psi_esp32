# ESP32-P4 libdatachannel component

# Source files for full build with WebSocket and Media
set(SOURCES
    # Core implementation files
    src/impl/init.cpp
    src/impl/certificate.cpp
    src/impl/channel.cpp
    src/impl/datachannel.cpp
    src/impl/dtlssrtptransport.cpp
    src/impl/dtlstransport.cpp
    src/impl/icetransport.cpp
    src/impl/peerconnection.cpp
    src/impl/processor.cpp
    src/impl/sctptransport.cpp
    src/impl/threadpool.cpp
    src/impl/tls.cpp
    src/impl/tlstransport.cpp
    src/impl/track.cpp
    src/impl/transport.cpp
    src/impl/utils.cpp
    src/impl/logcounter.cpp

    # WebSocket support
    src/impl/websocket.cpp
    src/impl/websocketserver.cpp
    src/impl/wstransport.cpp
    src/impl/wshandshake.cpp
    src/impl/tcptransport.cpp
    src/impl/pollservice.cpp
    src/impl/pollinterrupter.cpp
    src/impl/httpproxytransport.cpp
    src/impl/verifiedtlstransport.cpp
    src/impl/http.cpp
    src/impl/sha.cpp
    src/websocket.cpp
    src/websocketserver.cpp

    # Media support
    src/rtcpsrreporter.cpp
    src/rtcpreceivingsession.cpp
    src/rtcpnackresponder.cpp
    src/mediahandler.cpp
    src/rtppacketizer.cpp
    src/rtppacketizationconfig.cpp
    src/rtpdepacketizer.cpp
    src/rtp.cpp
    src/nalunit.cpp
    src/h264rtppacketizer.cpp
    src/h264rtpdepacketizer.cpp
    src/h265rtppacketizer.cpp
    src/h265rtpdepacketizer.cpp
    src/h265nalunit.cpp
    src/av1rtppacketizer.cpp
    src/dependencydescriptor.cpp
    src/plihandler.cpp
    src/rembhandler.cpp
    src/pacinghandler.cpp

    # ESP32 adaptations
    psram_allocator.cpp
    esp32_sockutils.cpp  # POSIX socket utilities (getifaddrs, getnameinfo)
    esp32_time.cpp
    esp32_random.cpp

    # Public API
    src/candidate.cpp
    src/channel.cpp
    src/configuration.cpp
    src/datachannel.cpp
    src/description.cpp
    src/message.cpp
    src/peerconnection.cpp
    src/track.cpp
    src/global.cpp
)

# ESP32 component registration
idf_component_register(
    SRCS ${SOURCES}
    INCLUDE_DIRS 
        include
        include/rtc
        src
    PRIV_INCLUDE_DIRS
        src/impl
        include/rtc
        deps/libsrtp/include
    REQUIRES
        pthread
        mbedtls
        lwip
        esp_netif
        esp_event
        esp_timer
        esp_wifi
        libsrtp
        esp_system
    PRIV_REQUIRES
        plog
        libjuice  
        usrsctp
)

# ESP32-specific compiler flags
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-switch  # Suppress warnings about unhandled enum values in switch statements
)

# Compiler definitions
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    RTC_ENABLE_WEBSOCKET=0     # Disable WebSocket to avoid static initialization issues on ESP32
    RTC_ENABLE_MEDIA=1
    USE_MBEDTLS=1
    ESP32_PORT=1
    CONFIG_MBEDTLS_SSL_PROTO_DTLS=1
    # Socket address length fields (must match usrsctp component)
    HAVE_SA_LEN=1
    HAVE_SIN_LEN=1
    HAVE_SIN6_LEN=1
    HAVE_SCONN_LEN=1
)

# MbedTLS X.509 certificate writing is already enabled in ESP-IDF

# Undefine ESP-IDF's removal of deprecated MbedTLS functions
target_compile_options(${COMPONENT_LIB} PRIVATE
    -UMBEDTLS_DEPRECATED_REMOVED
)

# ESP32-specific compiler flags
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-parameter
    -Wno-unused-variable
    -Wno-missing-field-initializers
)

# Use linker --wrap to override malloc/free/calloc/realloc
target_link_options(${COMPONENT_TARGET} INTERFACE
    "LINKER:--wrap=malloc"
    "LINKER:--wrap=free"
    "LINKER:--wrap=calloc"
    "LINKER:--wrap=realloc"
)