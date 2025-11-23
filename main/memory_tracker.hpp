// Memory allocation tracker for analyzing internal RAM usage
#pragma once

#include <cstddef>
#include <cstdint>

// Snapshot of memory state at a point in time
struct MemorySnapshot {
    size_t internal_free;
    size_t internal_min_free;
    size_t psram_free;
    size_t psram_min_free;
    size_t dma_free;
};

// Print detailed memory statistics with a label
void print_detailed_memory_stats(const char* label);

// Print memory change from baseline
void print_memory_delta(const char* label, const MemorySnapshot& baseline);

// Capture current memory state
MemorySnapshot capture_memory_snapshot();

// Print stack usage for all tasks
void print_task_stack_usage();

// Analyze heap fragmentation
void analyze_heap_fragmentation();

// ============================================================================
// LEAK TRACKER API - Simple, reusable leak detection
// ============================================================================

// Start leak tracking with configurable buffer sizes
// heap_trace_records: Track up to N allocations (all heaps: Internal + PSRAM)
// backtrace_records: Capture deep backtraces for up to N Internal RAM allocations
// Allocates both buffers from PSRAM to save Internal RAM
// Returns true on success, false if already running or allocation failed
bool leak_tracker_start(size_t heap_trace_records, size_t backtrace_records);

// Stop tracing, dump ONLY leaked allocations with backtraces, then free all trace memory
// Shows filtered output: only leaks with their full 12-level call stacks
void leak_tracker_end();
