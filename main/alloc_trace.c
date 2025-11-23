/*
 * Generic allocation tracer implementation
 * Captures allocations with deep backtraces using frame pointer unwinding
 */
#include "alloc_trace.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_cpu.h"

static const char* TAG = "alloc_trace";

// Frame pointer unwinding function (in ESP-IDF fp_unwind.c)
// Already marked ESP_SYSTEM_IRAM_ATTR so it's IRAM-safe
extern uint32_t esp_fp_get_callers(uint32_t frame, void** callers, void** stacks, uint32_t depth);

// ============================================================================
// Trace Buffer State
// ============================================================================
// Buffer is allocated dynamically from PSRAM to save Internal RAM
// Access to buffer is IRAM-safe even though buffer itself is in PSRAM

static alloc_trace_record_t* g_trace_buffer = NULL;
static size_t g_trace_capacity = 0;
static volatile int g_trace_count = 0;
static volatile bool g_trace_enabled = false;

// ============================================================================
// Frame Pointer Unwinding (IRAM)
// ============================================================================
// (Inlined into alloc_trace_record to avoid extra stack frame)

// ============================================================================
// Initialization & Control (Normal RAM - call from task context)
// ============================================================================

bool alloc_trace_init(size_t capacity) {
    if (g_trace_buffer != NULL) {
        ESP_LOGW(TAG, "Trace buffer already allocated, freeing old buffer");
        heap_caps_free(g_trace_buffer);
        g_trace_buffer = NULL;
        g_trace_capacity = 0;
    }

    if (capacity == 0) {
        ESP_LOGE(TAG, "Trace capacity must be > 0");
        return false;
    }

    // Try PSRAM first to save precious Internal RAM
    g_trace_buffer = (alloc_trace_record_t*)heap_caps_malloc(
        capacity * sizeof(alloc_trace_record_t),
        MALLOC_CAP_SPIRAM);

    if (!g_trace_buffer) {
        // Fallback to Internal RAM if PSRAM unavailable
        ESP_LOGW(TAG, "PSRAM not available, using Internal RAM for trace buffer");
        g_trace_buffer = (alloc_trace_record_t*)heap_caps_malloc(
            capacity * sizeof(alloc_trace_record_t),
            MALLOC_CAP_INTERNAL);
    }

    if (!g_trace_buffer) {
        ESP_LOGE(TAG, "Failed to allocate trace buffer (%zu bytes)",
                 capacity * sizeof(alloc_trace_record_t));
        return false;
    }

    g_trace_capacity = capacity;
    g_trace_count = 0;
    ESP_LOGI(TAG, "Initialized trace buffer: %zu records (%zu bytes)",
             capacity, capacity * sizeof(alloc_trace_record_t));
    return true;
}

void alloc_trace_cleanup(void) {
    if (g_trace_buffer) {
        heap_caps_free(g_trace_buffer);
        g_trace_buffer = NULL;
        g_trace_capacity = 0;
        g_trace_count = 0;
        g_trace_enabled = false;
        ESP_LOGI(TAG, "Trace buffer freed");
    }
}

void alloc_trace_start(void) {
    if (!g_trace_buffer) {
        ESP_LOGE(TAG, "Trace buffer not initialized! Call alloc_trace_init() first");
        return;
    }
    g_trace_count = 0;
    g_trace_enabled = true;
    ESP_LOGI(TAG, "Started tracing allocations");
}

void alloc_trace_stop(void) {
    g_trace_enabled = false;
    ESP_LOGI(TAG, "Stopped tracing, captured %d allocations", g_trace_count);
}

int alloc_trace_get_count(void) {
    return g_trace_count;
}

alloc_trace_record_t* alloc_trace_get_record(int index) {
    if (index >= 0 && index < g_trace_count) {
        return &g_trace_buffer[index];
    }
    return NULL;
}

// ============================================================================
// Recording (IRAM - can be called from anywhere)
// ============================================================================

// Record an allocation with backtrace (configurable depth)
// IRAM attribute makes this safe to call from ISRs or when cache is disabled
void HEAP_IRAM_ATTR alloc_trace_record(void* ptr, size_t size, uint32_t caps, uint32_t timestamp, int depth, void* caller) {
    // Quick exit if not capturing or buffer full
    // All checks use volatile variables so they're safe even from IRAM
    if (!ptr || !g_trace_enabled || !g_trace_buffer ||
        g_trace_count >= g_trace_capacity) {
        return;
    }

    // Clamp depth to valid range
    if (depth < 1) depth = 1;
    if (depth > ALLOC_TRACE_STACK_DEPTH) depth = ALLOC_TRACE_STACK_DEPTH;

    // Capture the record
    int idx = g_trace_count++;
    g_trace_buffer[idx].ptr = ptr;
    g_trace_buffer[idx].size = size;
    g_trace_buffer[idx].caps = caps;
    g_trace_buffer[idx].timestamp = timestamp;

    // Capture backtrace (IRAM-safe) with specified depth
    // Inlined to avoid extra stack frame
    if (depth == 1) {
        // For depth 1, use the provided caller address
        // This avoids frame pointer walking which can crash in some contexts
        g_trace_buffer[idx].callers[0] = caller;
        // Zero remaining slots
        for (int i = 1; i < ALLOC_TRACE_STACK_DEPTH; i++) {
            g_trace_buffer[idx].callers[i] = NULL;
        }
    } else {
        // For deeper traces, use frame pointer unwinding
        // We need to skip our own internal functions (alloc_trace_record, heap_caps_malloc)
        // So we request depth + 2 frames and copy starting from offset 2
        void* temp_callers[ALLOC_TRACE_STACK_DEPTH + 2];
        uint32_t fp;

        // Get current frame pointer (s0 register on RISC-V)
        __asm__ volatile ("mv %0, s0" : "=r"(fp));

        // Request extra frames to account for our internal functions
        uint32_t total_depth = depth + 2;
        if (total_depth > ALLOC_TRACE_STACK_DEPTH + 2) {
            total_depth = ALLOC_TRACE_STACK_DEPTH + 2;
        }

        // Use ESP-IDF's frame pointer walker (already IRAM-safe)
        uint32_t count = esp_fp_get_callers(fp, temp_callers, NULL, total_depth);

        // Copy frames, skipping first 2 (our internal functions)
        for (int i = 0; i < ALLOC_TRACE_STACK_DEPTH; i++) {
            if (i < depth && (i + 2) < count) {
                g_trace_buffer[idx].callers[i] = temp_callers[i + 2];
            } else {
                g_trace_buffer[idx].callers[i] = NULL;
            }
        }
    }
}
