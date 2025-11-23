/**
 * Operator new override to capture stack traces for memory allocations
 * This works because operator new is NOT in IRAM, so we can call backtrace APIs
 */

#include <new>
#include <cstdlib>
#include <cstdint>
#include "esp_heap_caps.h"
#include "esp_debug_helpers.h"
#include "esp_log.h"

static const char* TAG = "NewTrace";

#define MAX_NEW_TRACES 100
#define STACK_DEPTH 12

typedef struct {
    void* ptr;
    size_t size;
    void* callers[STACK_DEPTH];
    uint32_t timestamp;
} new_trace_t;

static new_trace_t g_new_traces[MAX_NEW_TRACES];
static volatile int g_new_trace_count = 0;
static volatile bool g_new_trace_enabled = false;

extern "C" void new_trace_start(void) {
    g_new_trace_count = 0;
    g_new_trace_enabled = true;
    ESP_LOGI(TAG, "Started tracing operator new calls");
}

extern "C" void new_trace_stop(void) {
    g_new_trace_enabled = false;
    ESP_LOGI(TAG, "Stopped tracing, captured %d allocations", g_new_trace_count);
}

extern "C" int new_trace_get_count(void) {
    return g_new_trace_count;
}

extern "C" new_trace_t* new_trace_get_record(int index) {
    if (index >= 0 && index < g_new_trace_count) {
        return &g_new_traces[index];
    }
    return nullptr;
}

// Capture stack trace using ESP-IDF eh_frame unwinding
static void capture_backtrace(void** callers, int depth) {
    esp_backtrace_frame_t frame;
    esp_backtrace_get_start(&frame.pc, &frame.sp, &frame.next_pc);

    int i = 0;
    callers[i++] = (void*)frame.pc;

    while (i < depth && esp_backtrace_get_next_frame(&frame)) {
        callers[i++] = (void*)frame.pc;
    }

    // Fill rest with NULL
    while (i < depth) {
        callers[i++] = nullptr;
    }
}

// Override operator new
void* operator new(std::size_t size) {
    void* ptr = malloc(size);

    if (!ptr) {
        throw std::bad_alloc();
    }

    // Capture trace if enabled and pointer is in tracked range
    if (g_new_trace_enabled && g_new_trace_count < MAX_NEW_TRACES) {
        uintptr_t addr = (uintptr_t)ptr;
        if (addr >= 0x4ff4d000 && addr <= 0x4ff5c000) {
            int idx = g_new_trace_count++;
            g_new_traces[idx].ptr = ptr;
            g_new_traces[idx].size = size;
            g_new_traces[idx].timestamp = esp_cpu_get_cycle_count();
            capture_backtrace(g_new_traces[idx].callers, STACK_DEPTH);
        }
    }

    return ptr;
}

// Override operator new[]
void* operator new[](std::size_t size) {
    return operator new(size);
}

// Override operator delete (don't trace, just free)
void operator delete(void* ptr) noexcept {
    free(ptr);
}

// Override operator delete[]
void operator delete[](void* ptr) noexcept {
    operator delete(ptr);
}

// Sized delete (C++14)
void operator delete(void* ptr, std::size_t size) noexcept {
    free(ptr);
}

void operator delete[](void* ptr, std::size_t size) noexcept {
    free(ptr);
}
