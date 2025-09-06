# ESP32-P4 usrsctp component

set(USRSCTP_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../libdatachannel/deps/usrsctp/usrsctplib)

# usrsctp source files (core userland SCTP implementation)
set(SOURCES
    ${USRSCTP_ROOT}/netinet/sctp_asconf.c
    ${USRSCTP_ROOT}/netinet/sctp_auth.c
    ${USRSCTP_ROOT}/netinet/sctp_bsd_addr.c
    ${USRSCTP_ROOT}/netinet/sctp_callout.c
    ${USRSCTP_ROOT}/netinet/sctp_cc_functions.c
    ${USRSCTP_ROOT}/netinet/sctp_crc32.c
    ${USRSCTP_ROOT}/netinet/sctp_indata.c
    ${USRSCTP_ROOT}/netinet/sctp_input.c
    ${USRSCTP_ROOT}/netinet/sctp_output.c
    ${USRSCTP_ROOT}/netinet/sctp_pcb.c
    ${USRSCTP_ROOT}/netinet/sctp_peeloff.c
    ${USRSCTP_ROOT}/netinet/sctp_sha1.c
    ${USRSCTP_ROOT}/netinet/sctp_ss_functions.c
    ${USRSCTP_ROOT}/netinet/sctp_sysctl.c
    ${USRSCTP_ROOT}/netinet/sctp_timer.c
    ${USRSCTP_ROOT}/netinet/sctp_userspace.c
    ${USRSCTP_ROOT}/netinet/sctp_usrreq.c
    ${USRSCTP_ROOT}/netinet/sctputil.c
    ${USRSCTP_ROOT}/netinet6/sctp6_usrreq.c
    ${USRSCTP_ROOT}/user_environment.c
    ${USRSCTP_ROOT}/user_mbuf.c
    ${USRSCTP_ROOT}/user_recv_thread.c
    ${USRSCTP_ROOT}/user_socket.c
)

# ESP32 component registration
idf_component_register(
    SRCS ${SOURCES}
    INCLUDE_DIRS 
        ${USRSCTP_ROOT}
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIV_INCLUDE_DIRS
        ${USRSCTP_ROOT}/netinet
        ${USRSCTP_ROOT}/netinet6
    REQUIRES 
        lwip
        pthread
)

# Compiler definitions for usrsctp
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    __Userspace__=1
    __Userspace_os_Linux=1
    SCTP_SIMPLE_ALLOCATOR=1
    SCTP_PROCESS_LEVEL_LOCKS=1
    SCTP_DEBUG=1
    ESP32_PORT=1
    _GNU_SOURCE=1
)

# Link-time wrapping for ESP32 compatibility  
target_link_options(${COMPONENT_LIB} PUBLIC
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
    -Wno-address-of-packed-member
    -Wno-format-truncation
    -Wno-implicit-fallthrough
)