# ESP32 libsrtp component (using MbedTLS backend)

set(LIBSRTP_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../components/libdatachannel/deps/libsrtp)

set(SRTP_SOURCES
    # Core SRTP
    ${LIBSRTP_ROOT}/srtp/srtp.c
    
    # Crypto kernel
    ${LIBSRTP_ROOT}/crypto/kernel/err.c
    ${LIBSRTP_ROOT}/crypto/kernel/alloc.c
    ${LIBSRTP_ROOT}/crypto/kernel/crypto_kernel.c
    ${LIBSRTP_ROOT}/crypto/kernel/key.c
    
    # Math
    ${LIBSRTP_ROOT}/crypto/math/datatypes.c
    
    # Cipher - MbedTLS variants
    ${LIBSRTP_ROOT}/crypto/cipher/cipher.c
    ${LIBSRTP_ROOT}/crypto/cipher/null_cipher.c
    ${LIBSRTP_ROOT}/crypto/cipher/aes.c
    ${LIBSRTP_ROOT}/crypto/cipher/aes_icm_mbedtls.c
    # Disable GCM temporarily - causing init failure
    # ${LIBSRTP_ROOT}/crypto/cipher/aes_gcm_mbedtls.c
    ${LIBSRTP_ROOT}/crypto/cipher/cipher_test_cases.c
    
    # Hash/Auth - MbedTLS variants
    ${LIBSRTP_ROOT}/crypto/hash/auth.c
    ${LIBSRTP_ROOT}/crypto/hash/null_auth.c
    ${LIBSRTP_ROOT}/crypto/hash/hmac_mbedtls.c
    ${LIBSRTP_ROOT}/crypto/hash/auth_test_cases.c
    
    # Replay protection
    ${LIBSRTP_ROOT}/crypto/replay/rdb.c
    ${LIBSRTP_ROOT}/crypto/replay/rdbx.c
)

idf_component_register(
    SRCS ${SRTP_SOURCES}
    INCLUDE_DIRS 
        ${LIBSRTP_ROOT}/include
        ${LIBSRTP_ROOT}/crypto/include
    REQUIRES
        mbedtls
)

# SRTP configuration for ESP32/MbedTLS
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    HAVE_CONFIG_H
    USE_MBEDTLS
    MBEDTLS
)

# Create config.h for libsrtp
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/config.h" "
/* ESP32 libsrtp configuration */
#define PACKAGE_VERSION \"2.5.0\"
#define PACKAGE_STRING \"libsrtp2 2.5.0\"

/* Use MbedTLS for crypto */
#define USE_MBEDTLS 1
/* Disable GCM temporarily to test */
/* #define GCM 1 */

/* Standard types */
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_STDINT_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_UNISTD_H 1
#define HAVE_ARPA_INET_H 1
#define HAVE_NETINET_IN_H 1
#define HAVE_SYS_TYPES_H 1

/* Features */
#define CPU_CISC 1
#define HAVE_INT32_T 1
#define HAVE_UINT32_T 1
#define HAVE_INT16_T 1  
#define HAVE_UINT16_T 1
#define HAVE_UINT64_T 1
#define HAVE_UINT8_T 1

/* ESP32 RISC-V supports 64-bit math - don't define NO_64BIT_MATH */
/* #undef NO_64BIT_MATH */

/* Disable other crypto backends */
/* #undef OPENSSL */
/* #undef NSS */

/* Endianness - ESP32 is little-endian, leave WORDS_BIGENDIAN undefined
   so that #ifndef WORDS_BIGENDIAN succeeds and uses little-endian bitfield layout */
/* #undef WORDS_BIGENDIAN */

/* Make sure we use standard types */
#define HAVE_STDINT_H 1
#define HAVE_INTTYPES_H 1
")

target_include_directories(${COMPONENT_LIB} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

# Force include config.h for all libsrtp sources
target_compile_options(${COMPONENT_LIB} PRIVATE
    -include ${CMAKE_CURRENT_BINARY_DIR}/config.h
    -Wno-format
    -Wno-incompatible-pointer-types
)