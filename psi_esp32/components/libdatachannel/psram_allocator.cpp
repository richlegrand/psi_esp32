// PSRAM allocation functions for libdatachannel component only
// These functions are called via preprocessor redirection in esp32_malloc_redirect.h
#include <cstddef>
#include <cstdlib>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_debug_helpers.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <new>

static const char* TAG = "rtc_psram";

// FreeRTOS TLS index for malloc target
// ESP-IDF reserves indices 0-1, pthread uses 0-4, so we use a high index to avoid conflicts
#define MALLOC_TARGET_TLS_INDEX (configNUM_THREAD_LOCAL_STORAGE_POINTERS - 1)

// Magic value to identify that TLS slot contains our caps value (not some other data)
#define TLS_CAPS_MAGIC 0xCAFEBABE

// Structure stored in TLS
struct tls_caps_data {
    uint32_t magic;
    uint32_t caps;
};

// Global default malloc target (starts as INTERNAL for early boot, switched to PSRAM later)
static uint32_t g_default_malloc_target = MALLOC_CAP_INTERNAL;

extern "C" {

// TLS deletion callback - called when task is deleted to free our TLS structure
static void tls_caps_delete_callback(int index, void* data) {
    if (data) {
        struct tls_caps_data* caps_data = (struct tls_caps_data*)data;
        if (caps_data->magic == TLS_CAPS_MAGIC) {
            heap_caps_free(data);
        }
    }
}

// Get current task's malloc target capability flags
uint32_t get_malloc_target() {
    TaskHandle_t task = xTaskGetCurrentTaskHandle();
    if (!task) {
        // No scheduler yet (early boot) - use global default
        return g_default_malloc_target;
    }

    void* tls_value = pvTaskGetThreadLocalStoragePointer(task, MALLOC_TARGET_TLS_INDEX);
    if (tls_value == NULL) {
        // Task hasn't set a preference - use global default
        return g_default_malloc_target;
    }

    // Verify it's actually our data (not some other component's data)
    struct tls_caps_data* data = (struct tls_caps_data*)tls_value;
    if (data->magic != TLS_CAPS_MAGIC) {
        // TLS slot contains something else, use global default
        return g_default_malloc_target;
    }

    return data->caps;
}

// Set current task's malloc target capability flags
void set_task_malloc_target(uint32_t caps) {
    TaskHandle_t task = xTaskGetCurrentTaskHandle();
    if (!task) {
        return;
    }

    void* tls_value = pvTaskGetThreadLocalStoragePointer(task, MALLOC_TARGET_TLS_INDEX);
    struct tls_caps_data* data = (struct tls_caps_data*)tls_value;

    // Check if we need to allocate TLS structure
    if (!data || data->magic != TLS_CAPS_MAGIC) {
        // Allocate from internal RAM to avoid recursion (we're inside malloc override!)
        data = (struct tls_caps_data*)heap_caps_malloc(sizeof(struct tls_caps_data), MALLOC_CAP_INTERNAL);
        if (!data) {
            return; // Allocation failed, can't set TLS
        }
        data->magic = TLS_CAPS_MAGIC;

        // Set TLS with deletion callback (automatically frees when task dies)
        vTaskSetThreadLocalStoragePointerAndDelCallback(task, MALLOC_TARGET_TLS_INDEX, data, tls_caps_delete_callback);
    }

    data->caps = caps;
}

// Enable PSRAM as default malloc target (called after PSRAM is initialized)
void enable_psram_malloc() {
    g_default_malloc_target = MALLOC_CAP_SPIRAM;
    ESP_LOGI(TAG, "PSRAM malloc enabled as global default");
}

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

// Debug: count internal RAM allocations and total malloc calls
static int g_internal_alloc_count = 0;
static size_t g_internal_alloc_bytes = 0;
static int g_malloc_call_count = 0;

// Global malloc override - uses FreeRTOS TLS to allow per-task control
// This intercepts STL allocations (std::make_shared, etc.) which call malloc directly
// Using __wrap_ prefix for linker --wrap mechanism
void* __wrap_malloc(size_t size) {
    g_malloc_call_count++;
    uint32_t caps = get_malloc_target();
    void* ptr = heap_caps_malloc(size, caps);
    if (!ptr && caps != MALLOC_CAP_INTERNAL) {
        // Fallback to internal RAM if target heap fails
        ptr = heap_caps_malloc(size, MALLOC_CAP_INTERNAL);
        if (ptr && g_default_malloc_target == MALLOC_CAP_SPIRAM) {
            // Count fallbacks
            g_internal_alloc_count++;
            g_internal_alloc_bytes += size;
        }
    }

    return ptr;
}

void __wrap_free(void* ptr) {
    heap_caps_free(ptr);
}

void* __wrap_calloc(size_t n, size_t size) {
    uint32_t caps = get_malloc_target();
    void* ptr = heap_caps_calloc(n, size, caps);
    if (!ptr && caps != MALLOC_CAP_INTERNAL) {
        ptr = heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL);
    }
    return ptr;
}

void* __wrap_realloc(void* ptr, size_t size) {
    uint32_t caps = get_malloc_target();
    void* new_ptr = heap_caps_realloc(ptr, size, caps);
    if (!new_ptr && size > 0 && caps != MALLOC_CAP_INTERNAL) {
        new_ptr = heap_caps_realloc(ptr, size, MALLOC_CAP_INTERNAL);
    }
    return new_ptr;
}

} // extern "C"

// Override global C++ operator new/delete to use PSRAM
// NOTE: Preprocessor redirection (malloc -> esp32_psram_malloc) does NOT affect
// C++ operator new! We must explicitly override these operators.
//
// This affects ALL C++ code in the application (libdatachannel, managed_components,
// application code, etc.). Any code using 'new' will allocate from PSRAM.
//
// If application code needs internal RAM for specific objects, use:
//   void* mem = heap_caps_malloc(sizeof(MyClass), MALLOC_CAP_INTERNAL);
//   MyClass* obj = new (mem) MyClass();  // placement new
//   delete obj;  // works correctly - heap_caps_free detects the region

#include <new>

// Forward declare our wrapped malloc (defined above)
extern "C" void* __wrap_malloc(size_t size);

// Debug counters for operator new
static int g_new_call_count = 0;
static size_t g_new_total_bytes = 0;

// Single-object allocation - use our TLS-aware malloc
void* operator new(size_t size) {
    g_new_call_count++;
    g_new_total_bytes += size;
    void* ptr = __wrap_malloc(size);
    if (!ptr) {
        throw std::bad_alloc();
    }

    return ptr;
}

// Array allocation - use our TLS-aware malloc
void* operator new[](size_t size) {
    void* ptr = __wrap_malloc(size);
    if (!ptr) {
        throw std::bad_alloc();
    }

    return ptr;
}

// Single-object deallocation
void operator delete(void* ptr) noexcept {
    heap_caps_free(ptr);
}

// Array deallocation
void operator delete[](void* ptr) noexcept {
    heap_caps_free(ptr);
}

// C++14 sized deallocation (single object)
void operator delete(void* ptr, size_t) noexcept {
    heap_caps_free(ptr);
}

// C++14 sized deallocation (array)
void operator delete[](void* ptr, size_t) noexcept {
    heap_caps_free(ptr);
}

// get_malloc_target is defined above in extern "C" block

// C++17 aligned allocation (single object) - use TLS-aware target
void* operator new(size_t size, std::align_val_t align) {
    uint32_t caps = get_malloc_target();
    void* ptr = heap_caps_aligned_alloc(static_cast<size_t>(align), size, caps);
    if (!ptr && caps != MALLOC_CAP_INTERNAL) {
        // Fallback to internal RAM if target fails
        ptr = heap_caps_aligned_alloc(static_cast<size_t>(align), size, MALLOC_CAP_INTERNAL);
    }
    if (!ptr) {
        throw std::bad_alloc();
    }
    return ptr;
}

// C++17 aligned allocation (array) - use TLS-aware target
void* operator new[](size_t size, std::align_val_t align) {
    uint32_t caps = get_malloc_target();
    void* ptr = heap_caps_aligned_alloc(static_cast<size_t>(align), size, caps);
    if (!ptr && caps != MALLOC_CAP_INTERNAL) {
        ptr = heap_caps_aligned_alloc(static_cast<size_t>(align), size, MALLOC_CAP_INTERNAL);
    }
    if (!ptr) {
        throw std::bad_alloc();
    }
    return ptr;
}

// C++17 aligned deallocation (single object)
void operator delete(void* ptr, std::align_val_t) noexcept {
    heap_caps_free(ptr);
}

// C++17 aligned deallocation (array)
void operator delete[](void* ptr, std::align_val_t) noexcept {
    heap_caps_free(ptr);
}

// C++17 sized aligned deallocation (single object)
void operator delete(void* ptr, size_t, std::align_val_t) noexcept {
    heap_caps_free(ptr);
}

// C++17 sized aligned deallocation (array)
void operator delete[](void* ptr, size_t, std::align_val_t) noexcept {
    heap_caps_free(ptr);
}

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

    // CRITICAL: Make child threads inherit this config!
    // Without this, threads created by library code (like usrsctp) will use default config (internal RAM)
    cfg.inherit_cfg = true;

    esp_err_t ret = esp_pthread_set_cfg(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure pthread for PSRAM: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Configured pthread to use PSRAM for thread stacks");
    }
}

extern "C" void esp32_ensure_pthread_psram(void) {
    // Check if pthread config is already set for this task/thread
    esp_pthread_cfg_t existing_cfg;
    esp_err_t ret = esp_pthread_get_cfg(&existing_cfg);

    if (ret == ESP_OK) {
        // Config already set - verify it's using PSRAM
        if (existing_cfg.stack_alloc_caps & MALLOC_CAP_SPIRAM) {
            // Already configured correctly
            return;
        }
    }

    // Not configured or using wrong caps - set PSRAM config
    esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
    cfg.stack_alloc_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    cfg.stack_size = 32768;  // 32KB
    cfg.inherit_cfg = true;

    ret = esp_pthread_set_cfg(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to ensure pthread PSRAM config: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Set pthread PSRAM config for current task");
    }
}

// Helper function to print memory stats
extern "C" void print_rtc_memory_stats() {
    ESP_LOGI(TAG, "PSRAM free: %d KB, Internal free: %d KB",
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024,
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
}

// Debug: print allocation statistics
extern "C" void print_alloc_stats() {
    ESP_LOGI(TAG, "=== Allocation Statistics ===");
    ESP_LOGI(TAG, "operator new calls: %d, total bytes: %zu", g_new_call_count, g_new_total_bytes);
    ESP_LOGI(TAG, "__wrap_malloc calls: %d", g_malloc_call_count);
    ESP_LOGI(TAG, "PSRAM fallbacks to internal: %d, total bytes: %zu", g_internal_alloc_count, g_internal_alloc_bytes);
}

// Debug: reset allocation statistics
extern "C" void reset_alloc_stats() {
    g_new_call_count = 0;
    g_new_total_bytes = 0;
    g_malloc_call_count = 0;
    g_internal_alloc_count = 0;
    g_internal_alloc_bytes = 0;
}