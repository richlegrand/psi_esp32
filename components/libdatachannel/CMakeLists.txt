# ESP32-P4 libdatachannel component

set(LIBDATACHANNEL_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../libdatachannel)

# Source files for minimal build
set(SOURCES
    # Core implementation files
    ${LIBDATACHANNEL_ROOT}/src/impl/init.cpp
    ${LIBDATACHANNEL_ROOT}/src/impl/certificate.cpp
    ${LIBDATACHANNEL_ROOT}/src/impl/channel.cpp
    ${LIBDATACHANNEL_ROOT}/src/impl/datachannel.cpp
    ${LIBDATACHANNEL_ROOT}/src/impl/dtlstransport.cpp
    ${LIBDATACHANNEL_ROOT}/src/impl/icetransport.cpp
    ${LIBDATACHANNEL_ROOT}/src/impl/peerconnection.cpp
    ${LIBDATACHANNEL_ROOT}/src/impl/processor.cpp
    ${LIBDATACHANNEL_ROOT}/src/impl/sctptransport.cpp
    ${LIBDATACHANNEL_ROOT}/src/impl/transport.cpp
    ${LIBDATACHANNEL_ROOT}/src/impl/utils.cpp
    ${LIBDATACHANNEL_ROOT}/src/impl/logcounter.cpp
    
    # ESP32 adaptations
    psram_allocator.cpp
    esp32_netif.cpp
    esp32_time.cpp
    esp32_random.cpp
    
    # Public API
    ${LIBDATACHANNEL_ROOT}/src/candidate.cpp
    ${LIBDATACHANNEL_ROOT}/src/channel.cpp
    ${LIBDATACHANNEL_ROOT}/src/configuration.cpp
    ${LIBDATACHANNEL_ROOT}/src/datachannel.cpp
    ${LIBDATACHANNEL_ROOT}/src/description.cpp
    ${LIBDATACHANNEL_ROOT}/src/message.cpp
    ${LIBDATACHANNEL_ROOT}/src/peerconnection.cpp
    ${LIBDATACHANNEL_ROOT}/src/reliability.cpp
)

# ESP32 component registration
idf_component_register(
    SRCS ${SOURCES}
    INCLUDE_DIRS 
        ${LIBDATACHANNEL_ROOT}/include
        ${LIBDATACHANNEL_ROOT}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIV_INCLUDE_DIRS
        ${LIBDATACHANNEL_ROOT}/src/impl
    REQUIRES 
        pthread
        mbedtls
        lwip
        esp_netif
        esp_event
    PRIV_REQUIRES
        plog
        libjuice  
        usrsctp
)

# Compiler definitions
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    RTC_ENABLE_WEBSOCKET=0
    RTC_ENABLE_MEDIA=0
    USE_MBEDTLS=1
    ESP32_PORT=1
)

# Link-time wrapping for ESP32 compatibility (only for this component)
target_link_options(${COMPONENT_LIB} PUBLIC
    # Memory allocation -> PSRAM
    -Wl,--wrap=malloc
    -Wl,--wrap=free
    -Wl,--wrap=calloc
    -Wl,--wrap=realloc
    -Wl,--wrap=_Znwm     # operator new(size_t)
    -Wl,--wrap=_Znam     # operator new[](size_t) 
    -Wl,--wrap=_ZdlPv    # operator delete(void*)
    -Wl,--wrap=_ZdaPv    # operator delete[](void*)
    # File I/O -> ESP32 RNG
    -Wl,--wrap=open
    -Wl,--wrap=read
    -Wl,--wrap=close
)

# ESP32-specific compiler flags
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-parameter
    -Wno-unused-variable
    -Wno-missing-field-initializers
)