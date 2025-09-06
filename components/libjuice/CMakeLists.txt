# ESP32-P4 libjuice component

set(LIBJUICE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../libdatachannel/deps/libjuice)

# libjuice source files
set(SOURCES
    ${LIBJUICE_ROOT}/src/addr.c
    ${LIBJUICE_ROOT}/src/agent.c
    ${LIBJUICE_ROOT}/src/crc32.c
    ${LIBJUICE_ROOT}/src/hash.c
    ${LIBJUICE_ROOT}/src/hmac.c
    ${LIBJUICE_ROOT}/src/ice.c
    ${LIBJUICE_ROOT}/src/juice.c
    ${LIBJUICE_ROOT}/src/log.c
    ${LIBJUICE_ROOT}/src/random.c
    ${LIBJUICE_ROOT}/src/server.c
    ${LIBJUICE_ROOT}/src/stun.c
    ${LIBJUICE_ROOT}/src/timestamp.c
    ${LIBJUICE_ROOT}/src/turn.c
    ${LIBJUICE_ROOT}/src/udp.c
)

# ESP32 component registration
idf_component_register(
    SRCS ${SOURCES}
    INCLUDE_DIRS 
        ${LIBJUICE_ROOT}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIV_INCLUDE_DIRS
        ${LIBJUICE_ROOT}/src
    REQUIRES 
        lwip
        pthread
        mbedtls
)

# Compiler definitions for libjuice
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    JUICE_USE_NETTLE=0
    JUICE_ENABLE_DEBUG=1
    NO_EPOLL=1
    ESP32_PORT=1
)

# Link-time wrapping for ESP32 compatibility
target_link_options(${COMPONENT_LIB} PUBLIC
    # File I/O -> ESP32 RNG
    -Wl,--wrap=open
    -Wl,--wrap=read
    -Wl,--wrap=close
    # Memory allocation -> PSRAM
    -Wl,--wrap=malloc
    -Wl,--wrap=free
    -Wl,--wrap=calloc
    -Wl,--wrap=realloc
)

# ESP32-specific compiler flags
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-parameter
    -Wno-unused-variable
    -Wno-missing-field-initializers
    -Wno-format-truncation
)