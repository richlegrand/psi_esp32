// PSRAM allocation functions for libdatachannel component only
// These functions are called via preprocessor redirection in esp32_malloc_redirect.h
#include <cstddef>
#include <cstdlib>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_system.h>

static const char* TAG = "rtc_psram";

extern "C" {

// PSRAM allocation functions - only called by libdatachannel code
void* esp32_psram_malloc(size_t size) {
    void* ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (!ptr) {
        // Fallback to internal RAM if PSRAM allocation fails
        ptr = heap_caps_malloc(size, MALLOC_CAP_INTERNAL);
    }
    return ptr;
}

void esp32_psram_free(void* ptr) {
    heap_caps_free(ptr);
}

void* esp32_psram_calloc(size_t n, size_t size) {
    void* ptr = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM);
    if (!ptr) {
        ptr = heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL);
    }
    return ptr;
}

void* esp32_psram_realloc(void* ptr, size_t size) {
    // Try PSRAM first
    void* new_ptr = heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM);
    if (!new_ptr && size > 0) {
        // Fallback to internal
        new_ptr = heap_caps_realloc(ptr, size, MALLOC_CAP_INTERNAL);
    }
    return new_ptr;
}

} // extern "C"

// Note: C++ operators new/delete will automatically use our redirected malloc/free
// via the preprocessor macros in esp32_malloc_redirect.h, so no special handling needed

// Configure pthread to use PSRAM for thread stacks
#include <pthread.h>
#include <esp_pthread.h>

extern "C" void esp32_configure_pthread_psram(void) {
    esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
    
    // Use PSRAM for thread stacks (critical for usrsctp which creates many threads)
    cfg.stack_alloc_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    
    // Set a larger default stack size for libdatachannel threads
    // Many WebRTC/SCTP operations are stack-intensive
    cfg.stack_size = 32768;  // 32KB default, will be allocated from PSRAM
    
    esp_err_t ret = esp_pthread_set_cfg(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure pthread for PSRAM: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Configured pthread to use PSRAM for thread stacks");
    }
}

// Helper function to print memory stats
extern "C" void print_rtc_memory_stats() {
    ESP_LOGI(TAG, "PSRAM free: %d KB, Internal free: %d KB", 
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024,
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
}