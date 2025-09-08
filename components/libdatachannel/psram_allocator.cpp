// PSRAM allocation wrappers for libdatachannel component only
// These functions are linked via --wrap linker flags
#include <cstddef>
#include <cstdlib>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_system.h>

static const char* TAG = "rtc_psram";

extern "C" {

// Wrapped malloc functions - only called by libdatachannel code
void* __real_malloc(size_t size);
void* __wrap_malloc(size_t size) {
    void* ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (!ptr) {
        ESP_LOGW(TAG, "PSRAM alloc failed for %zu bytes, trying internal", size);
        ptr = heap_caps_malloc(size, MALLOC_CAP_INTERNAL);
    }
    return ptr;
}

void __real_free(void* ptr);
void __wrap_free(void* ptr) {
    heap_caps_free(ptr);
}

void* __real_calloc(size_t n, size_t size);
void* __wrap_calloc(size_t n, size_t size) {
    void* ptr = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM);
    if (!ptr) {
        ptr = heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL);
    }
    return ptr;
}

void* __real_realloc(void* ptr, size_t size);
void* __wrap_realloc(void* ptr, size_t size) {
    // Try PSRAM first
    void* new_ptr = heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM);
    if (!new_ptr && size > 0) {
        // Fallback to internal
        new_ptr = heap_caps_realloc(ptr, size, MALLOC_CAP_INTERNAL);
    }
    return new_ptr;
}

} // extern "C"

// Wrapped C++ operators - mangled names
extern "C" {

// operator new(size_t) - mangled as _Znwm
void* __real__Znwm(std::size_t size);
void* __wrap__Znwm(std::size_t size) {
    void* ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (!ptr) {
        ESP_LOGW(TAG, "PSRAM new failed for %zu bytes, trying internal", size);
        ptr = heap_caps_malloc(size, MALLOC_CAP_INTERNAL);
        if (!ptr) {
            ESP_LOGE(TAG, "Critical: Out of memory for %zu bytes allocation", size);
            esp_system_abort("libdatachannel: Out of memory");
        }
    }
    return ptr;
}

// operator new[](size_t) - mangled as _Znam
void* __real__Znam(std::size_t size);
void* __wrap__Znam(std::size_t size) {
    return __wrap__Znwm(size);
}

// operator delete(void*) - mangled as _ZdlPv
void __real__ZdlPv(void* ptr);
void __wrap__ZdlPv(void* ptr) {
    heap_caps_free(ptr);
}

// operator delete[](void*) - mangled as _ZdaPv
void __real__ZdaPv(void* ptr);
void __wrap__ZdaPv(void* ptr) {
    heap_caps_free(ptr);
}

} // extern "C"

// Helper function to print memory stats
void print_rtc_memory_stats() {
    ESP_LOGI(TAG, "PSRAM free: %d KB, Internal free: %d KB", 
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024,
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
}