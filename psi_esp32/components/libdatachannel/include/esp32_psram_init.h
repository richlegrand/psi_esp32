// ESP32 PSRAM initialization for libdatachannel
// pthread PSRAM configuration is handled automatically during library initialization
// Applications do not need to call esp32_configure_pthread_psram() directly

#ifndef ESP32_PSRAM_INIT_H
#define ESP32_PSRAM_INIT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Configure pthread to use PSRAM for thread stacks
// Called automatically during rtc::StartNetworking() - applications should not call directly
void esp32_configure_pthread_psram(void);

// Ensure pthread is configured for PSRAM in the current thread/task
// This is idempotent - safe to call multiple times, from any thread
// Call this before spawning std::thread from non-pthread contexts (e.g., FreeRTOS tasks)
void esp32_ensure_pthread_psram(void);

// Enable PSRAM as default malloc target (call after PSRAM is initialized in app_main)
// Before this is called, malloc uses INTERNAL RAM (safe for early boot)
void enable_psram_malloc(void);

// Set malloc target for current task (uses FreeRTOS TLS)
// Use MALLOC_CAP_INTERNAL for specific allocations that need internal RAM.
// Note: This affects the current task only
void set_task_malloc_target(uint32_t caps);

// Print memory statistics
void print_rtc_memory_stats(void);

// Debug: Print allocation statistics (operator new, malloc, fallbacks)
void print_alloc_stats(void);

// Debug: Reset allocation statistics
void reset_alloc_stats(void);

#ifdef __cplusplus
}
#endif

#endif // ESP32_PSRAM_INIT_H