/*
 * Generic allocation tracer with deep backtraces
 *
 * Captures allocations from any allocator (pvPortMalloc, heap_caps_malloc, etc.)
 * with full frame pointer backtraces for leak debugging.
 *
 * IRAM-safe: Can be called from ISRs or when flash cache is disabled
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_attr.h"
#include "esp_heap_caps.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ALLOC_TRACE_STACK_DEPTH 24

typedef struct {
    void* ptr;                              // Allocated address
    size_t size;                            // Allocation size in bytes
    uint32_t caps;                          // Memory capability flags (MALLOC_CAP_*)
    void* callers[ALLOC_TRACE_STACK_DEPTH]; // Call stack backtrace
    uint32_t timestamp;                     // CPU cycle count when allocated
} alloc_trace_record_t;

// ============================================================================
// Initialization & Control (NOT IRAM - call from normal context only)
// ============================================================================

// Initialize trace buffer with given capacity
// Allocates from PSRAM if available to save Internal RAM
// Returns true on success, false on failure
bool alloc_trace_init(size_t capacity);

// Free trace buffer and reset all state
void alloc_trace_cleanup(void);

// Start capturing allocations (resets count to 0)
void alloc_trace_start(void);

// Stop capturing allocations
void alloc_trace_stop(void);

// Get number of captured allocations
int alloc_trace_get_count(void);

// Get trace record by index (0-based)
// Returns NULL if index out of range
alloc_trace_record_t* alloc_trace_get_record(int index);

// ============================================================================
// Recording (IRAM-safe - can be called from anywhere)
// ============================================================================

// Record an allocation with backtrace
// This is IRAM-safe and can be called from pvPortMalloc, heap_caps_malloc, ISRs, etc.
//
// Parameters:
//   ptr       - Allocated address (if NULL, ignored)
//   size      - Allocation size in bytes
//   caps      - Memory capability flags (MALLOC_CAP_INTERNAL, MALLOC_CAP_SPIRAM, etc.)
//   timestamp - CPU cycle count (typically from esp_cpu_get_cycle_count())
//   depth     - Stack depth to capture (1 = use caller param, 12 = full backtrace)
//   caller    - Caller address (use __builtin_return_address(0) from allocator)
void HEAP_IRAM_ATTR alloc_trace_record(void* ptr, size_t size, uint32_t caps, uint32_t timestamp, int depth, void* caller);

#ifdef __cplusplus
}
#endif
