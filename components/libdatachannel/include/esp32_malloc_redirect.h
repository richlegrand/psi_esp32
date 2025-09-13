// ESP32 PSRAM allocation redirects for libdatachannel
// This header is force-included when compiling libdatachannel source files
// to redirect allocations to PSRAM

#ifndef ESP32_MALLOC_REDIRECT_H
#define ESP32_MALLOC_REDIRECT_H

#include <stddef.h>  // for size_t

// Include standard headers first to avoid macro conflicts
#ifdef __cplusplus
#include <cstdlib>
#include <cstring>
#else
#include <stdlib.h>
#include <string.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Custom allocation functions that use PSRAM
void* esp32_psram_malloc(size_t size);
void esp32_psram_free(void* ptr);
void* esp32_psram_calloc(size_t n, size_t size);
void* esp32_psram_realloc(void* ptr, size_t size);

#ifdef __cplusplus
}
#endif

// Redirect C allocation functions to our PSRAM versions
// Do this AFTER standard headers are included to avoid breaking namespace imports
#define malloc(size) esp32_psram_malloc(size)
#define free(ptr) esp32_psram_free(ptr)
#define calloc(n, size) esp32_psram_calloc(n, size)
#define realloc(ptr, size) esp32_psram_realloc(ptr, size)

#endif // ESP32_MALLOC_REDIRECT_H