// Memory allocation tracker for analyzing internal RAM usage
// This helps identify which components are allocating from internal RAM vs PSRAM

#include "memory_tracker.hpp"
#include <esp_heap_caps.h>
#include <esp_heap_trace.h>
#include <esp_log.h>
#include <esp_memory_utils.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char* TAG = "mem_track";

// Compile-time check for frame pointer support
#ifndef CONFIG_ESP_SYSTEM_USE_FRAME_POINTER
#error "Frame pointer unwinding required! Enable CONFIG_ESP_SYSTEM_USE_FRAME_POINTER in menuconfig"
#error "  -> Component config -> ESP System Settings -> Use frame pointer for backtracing"
#endif

// Include allocation tracer
#include "alloc_trace.h"

// ============================================================================
// Leak Tracker State
// ============================================================================
static heap_trace_record_t* leak_trace_buffer = nullptr;
static size_t leak_trace_size = 0;
static bool leak_tracking_active = false;

// Print detailed memory statistics by capability
void print_detailed_memory_stats(const char* label) {
    ESP_LOGI(TAG, "=== Memory Stats: %s ===", label);

    // Total heap info
    multi_heap_info_t info;
    heap_caps_get_info(&info, MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "Internal RAM:");
    ESP_LOGI(TAG, "  Total:      %6d KB", info.total_allocated_bytes / 1024);
    ESP_LOGI(TAG, "  Free:       %6d KB", heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
    ESP_LOGI(TAG, "  Allocated:  %6d KB", info.total_allocated_bytes / 1024);
    ESP_LOGI(TAG, "  Min free:   %6d KB", heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024);
    ESP_LOGI(TAG, "  Blocks:     %6d", info.allocated_blocks);
    ESP_LOGI(TAG, "  Largest:    %6d KB", heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024);

    heap_caps_get_info(&info, MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "PSRAM:");
    ESP_LOGI(TAG, "  Total:      %6d KB", info.total_allocated_bytes / 1024);
    ESP_LOGI(TAG, "  Free:       %6d KB", heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
    ESP_LOGI(TAG, "  Allocated:  %6d KB", info.total_allocated_bytes / 1024);
    ESP_LOGI(TAG, "  Min free:   %6d KB", heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM) / 1024);
    ESP_LOGI(TAG, "  Blocks:     %6d", info.allocated_blocks);
    ESP_LOGI(TAG, "  Largest:    %6d KB", heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024);

    // DMA-capable memory
    ESP_LOGI(TAG, "DMA-capable: %6d KB free",
             heap_caps_get_free_size(MALLOC_CAP_DMA) / 1024);

    ESP_LOGI(TAG, "================================");
}

// Print memory snapshot showing change from baseline
void print_memory_delta(const char* label, const MemorySnapshot& baseline) {
    MemorySnapshot current = capture_memory_snapshot();

    int internal_delta = (int)baseline.internal_free - (int)current.internal_free;
    int psram_delta = (int)baseline.psram_free - (int)current.psram_free;
    int dma_delta = (int)baseline.dma_free - (int)current.dma_free;

    ESP_LOGI(TAG, "=== Memory Delta: %s ===", label);
    ESP_LOGI(TAG, "Internal: %+6d KB (now %6d KB free)",
             internal_delta / 1024, current.internal_free / 1024);
    ESP_LOGI(TAG, "PSRAM:    %+6d KB (now %6d KB free)",
             psram_delta / 1024, current.psram_free / 1024);
    ESP_LOGI(TAG, "DMA:      %+6d KB (now %6d KB free)",
             dma_delta / 1024, current.dma_free / 1024);
    ESP_LOGI(TAG, "=============================");
}

// Capture current memory state
MemorySnapshot capture_memory_snapshot() {
    MemorySnapshot snap;
    snap.internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    snap.internal_min_free = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    snap.psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    snap.psram_min_free = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
    snap.dma_free = heap_caps_get_free_size(MALLOC_CAP_DMA);
    return snap;
}

// Print all task stack high water marks
void print_task_stack_usage() {
    ESP_LOGI(TAG, "=== Task Stack Usage ===");

    UBaseType_t task_count = uxTaskGetNumberOfTasks();
    TaskStatus_t* task_status_array = (TaskStatus_t*)heap_caps_malloc(
        task_count * sizeof(TaskStatus_t), MALLOC_CAP_INTERNAL);

    if (task_status_array) {
        UBaseType_t actual_count = uxTaskGetSystemState(task_status_array, task_count, NULL);

        for (UBaseType_t i = 0; i < actual_count; i++) {
            uint32_t stack_size = task_status_array[i].usStackHighWaterMark * sizeof(StackType_t);
            ESP_LOGI(TAG, "  %-20s: %5d bytes free (of configured stack)",
                     task_status_array[i].pcTaskName,
                     stack_size);
        }

        heap_caps_free(task_status_array);
    }

    ESP_LOGI(TAG, "========================");
}

// Analyze heap fragmentation
void analyze_heap_fragmentation() {
    ESP_LOGI(TAG, "=== Heap Fragmentation ===");

    multi_heap_info_t info;
    heap_caps_get_info(&info, MALLOC_CAP_INTERNAL);

    size_t largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    size_t total_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

    float fragmentation = 100.0f * (1.0f - ((float)largest_block / (float)total_free));

    ESP_LOGI(TAG, "Internal RAM:");
    ESP_LOGI(TAG, "  Total free:    %6d KB", total_free / 1024);
    ESP_LOGI(TAG, "  Largest block: %6d KB", largest_block / 1024);
    ESP_LOGI(TAG, "  Blocks:        %6d", info.free_blocks);
    ESP_LOGI(TAG, "  Fragmentation: %5.1f%%", fragmentation);

    heap_caps_get_info(&info, MALLOC_CAP_SPIRAM);
    largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    total_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    fragmentation = 100.0f * (1.0f - ((float)largest_block / (float)total_free));

    ESP_LOGI(TAG, "PSRAM:");
    ESP_LOGI(TAG, "  Total free:    %6d KB", total_free / 1024);
    ESP_LOGI(TAG, "  Largest block: %6d KB", largest_block / 1024);
    ESP_LOGI(TAG, "  Blocks:        %6d", info.free_blocks);
    ESP_LOGI(TAG, "  Fragmentation: %5.1f%%", fragmentation);

    ESP_LOGI(TAG, "==========================");
}

// ============================================================================
// LEAK TRACKER API - Simple, reusable leak detection
// ============================================================================

bool leak_tracker_start(size_t heap_trace_records, size_t backtrace_records)
{
    // Verify frame pointers are enabled at runtime
#ifndef CONFIG_ESP_SYSTEM_USE_FRAME_POINTER
    ESP_LOGE(TAG, "========================================");
    ESP_LOGE(TAG, "ERROR: Frame pointers are NOT enabled!");
    ESP_LOGE(TAG, "========================================");
    ESP_LOGE(TAG, "Backtrace capture requires frame pointer unwinding.");
    ESP_LOGE(TAG, "");
    ESP_LOGE(TAG, "To enable:");
    ESP_LOGE(TAG, "  1. Run: idf.py menuconfig");
    ESP_LOGE(TAG, "  2. Navigate to: Component config -> ESP System Settings");
    ESP_LOGE(TAG, "  3. Enable: [ ] Use frame pointer for backtracing");
    ESP_LOGE(TAG, "  4. Rebuild: idf.py build");
    ESP_LOGE(TAG, "========================================");
    return false;
#endif

    if (leak_tracking_active) {
        ESP_LOGW(TAG, "Leak tracker already running");
        return false;
    }

    // Allocate heap trace buffer from PSRAM (saves precious Internal RAM)
    leak_trace_buffer = (heap_trace_record_t*)heap_caps_malloc(
        heap_trace_records * sizeof(heap_trace_record_t),
        MALLOC_CAP_SPIRAM);

    if (!leak_trace_buffer) {
        ESP_LOGE(TAG, "Failed to allocate heap trace buffer (%zu bytes)",
                 heap_trace_records * sizeof(heap_trace_record_t));
        return false;
    }

    leak_trace_size = heap_trace_records;

    // Initialize and start heap trace in LEAKS mode
    // In LEAKS mode, freed allocations are automatically removed from the trace
    esp_err_t err = heap_trace_init_standalone(leak_trace_buffer, heap_trace_records);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "heap_trace_init_standalone failed: %s", esp_err_to_name(err));
        heap_caps_free(leak_trace_buffer);
        leak_trace_buffer = nullptr;
        return false;
    }

    err = heap_trace_start(HEAP_TRACE_LEAKS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "heap_trace_start failed: %s", esp_err_to_name(err));
        heap_caps_free(leak_trace_buffer);
        leak_trace_buffer = nullptr;
        return false;
    }

    // Initialize allocation backtrace buffer
    // This allocates from PSRAM for the backtrace buffer
    if (!alloc_trace_init(backtrace_records)) {
        ESP_LOGE(TAG, "Failed to initialize backtrace buffer");
        heap_trace_stop();
        heap_caps_free(leak_trace_buffer);
        leak_trace_buffer = nullptr;
        return false;
    }

    // Start allocation backtrace capture
    alloc_trace_start();

    leak_tracking_active = true;
    ESP_LOGI(TAG, "Leak tracker started (heap_trace: %zu, backtraces: %zu)",
             heap_trace_records, backtrace_records);
    return true;
}

void leak_tracker_end()
{
    if (!leak_tracking_active) {
        ESP_LOGW(TAG, "Leak tracker not running");
        return;
    }

    // Stop both traces
    heap_trace_stop();
    alloc_trace_stop();

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "   LEAKED ALLOCATIONS");
    ESP_LOGI(TAG, "========================================");

    // Get counts
    size_t heap_leaked_count = heap_trace_get_count();
    int backtrace_count = alloc_trace_get_count();

    ESP_LOGI(TAG, "Heap trace leak records: %zu", heap_leaked_count);
    ESP_LOGI(TAG, "Backtraces captured: %d", backtrace_count);
    ESP_LOGI(TAG, "");

    size_t total_leaked_bytes = 0;
    size_t leaked_internal = 0;
    size_t leaked_psram = 0;
    size_t backtraces_found = 0;

    // Iterate through heap trace records
    // In LEAKS mode, all records are leaks (freed ones are auto-removed)
    for (size_t i = 0; i < heap_leaked_count; i++) {
        heap_trace_record_t record;
        esp_err_t err = heap_trace_get(i, &record);

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to get heap trace record %zu: %s", i, esp_err_to_name(err));
            continue;
        }

        // Sanity check (should not happen in LEAKS mode, but be defensive)
        if (record.address == NULL) {
            continue;
        }

        total_leaked_bytes += record.size;

        // Categorize by memory type
        bool is_internal = esp_ptr_internal(record.address);
        bool is_psram = esp_ptr_external_ram(record.address);

        if (is_internal) leaked_internal++;
        if (is_psram) leaked_psram++;

        const char* mem_label = is_internal ? "Internal" : (is_psram ? "PSRAM" : "Unknown");

        ESP_LOGI(TAG, "--- LEAK #%zu ---", i + 1);
        ESP_LOGI(TAG, "Address: %p (%s)", record.address, mem_label);
        ESP_LOGI(TAG, "Size: %u bytes", record.size);

        // Try to find matching allocation backtrace
        alloc_trace_record_t* backtrace = nullptr;
        for (int j = 0; j < backtrace_count; j++) {
            alloc_trace_record_t* trace = alloc_trace_get_record(j);
            if (trace && trace->ptr == record.address) {
                backtrace = trace;
                backtraces_found++;
                break;
            }
        }

        if (backtrace) {
            ESP_LOGI(TAG, "Backtrace (frame pointer unwinding):");
            ESP_LOGI(TAG, "  %p:%p:%p:%p:%p:%p",
                     backtrace->callers[0], backtrace->callers[1],
                     backtrace->callers[2], backtrace->callers[3],
                     backtrace->callers[4], backtrace->callers[5]);
            ESP_LOGI(TAG, "  %p:%p:%p:%p:%p:%p",
                     backtrace->callers[6], backtrace->callers[7],
                     backtrace->callers[8], backtrace->callers[9],
                     backtrace->callers[10], backtrace->callers[11]);
            ESP_LOGI(TAG, "  %p:%p:%p:%p:%p:%p",
                     backtrace->callers[12], backtrace->callers[13],
                     backtrace->callers[14], backtrace->callers[15],
                     backtrace->callers[16], backtrace->callers[17]);
            ESP_LOGI(TAG, "  %p:%p:%p:%p:%p:%p",
                     backtrace->callers[18], backtrace->callers[19],
                     backtrace->callers[20], backtrace->callers[21],
                     backtrace->callers[22], backtrace->callers[23]);
        } else {
            ESP_LOGI(TAG, "Backtrace: (not captured - buffer full or not FreeRTOS heap)");
        }
        ESP_LOGI(TAG, "");
    }

    // Summary
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "   LEAK SUMMARY");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "Total leaks: %zu allocations, %zu bytes (%.2f KB)",
             heap_leaked_count, total_leaked_bytes, total_leaked_bytes / 1024.0);
    ESP_LOGI(TAG, "  Internal RAM: %zu allocations", leaked_internal);
    ESP_LOGI(TAG, "  PSRAM: %zu allocations", leaked_psram);
    ESP_LOGI(TAG, "  Backtraces found: %zu / %zu", backtraces_found, heap_leaked_count);
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "Current heap status:");
    ESP_LOGI(TAG, "  Internal free: %u KB", heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
    ESP_LOGI(TAG, "  PSRAM free: %u KB", heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
    ESP_LOGI(TAG, "========================================");

    // Cleanup - free both trace buffers
    alloc_trace_cleanup();
    heap_caps_free(leak_trace_buffer);
    leak_trace_buffer = nullptr;
    leak_trace_size = 0;
    leak_tracking_active = false;

    ESP_LOGI(TAG, "Leak tracker stopped and cleaned up");
}
