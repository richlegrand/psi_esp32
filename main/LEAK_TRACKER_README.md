# Memory Leak Tracker for ESP32

A memory leak detection tool for ESP32 that combines ESP-IDF's heap trace with custom backtrace capture to identify leaked allocations and their origins.

## Overview

The leak tracker wraps ESP-IDF's `heap_trace` functionality with additional features:
- Backtrace capture using frame pointer unwinding
- Allocation tracking in PSRAM (to save Internal RAM)
- Integration with ESP32 address decoding
- Detailed leak reports with source locations

## Files

- `memory_tracker.hpp` - Header with API declarations
- `memory_tracker.cpp` - Main leak tracking implementation

## API

### Starting Leak Tracking

```cpp
#include "memory_tracker.hpp"

// Start tracking with default buffer sizes (1000 records each)
leak_tracker_start();

// Or specify custom buffer sizes
leak_tracker_start(3000, 3000);  // heap_trace_records, backtrace_records
```

**Parameters:**
- `heap_trace_records` - Number of allocations to track in ESP-IDF heap trace
- `backtrace_records` - Number of backtraces to capture

**Buffer allocation:**
- Both buffers are allocated in PSRAM to preserve Internal RAM
- Each heap trace record: ~112 bytes
- Each backtrace record: variable (depends on stack depth)

### Stopping and Reporting

```cpp
// Stop tracking and print leak report
leak_tracker_end();
```

This will:
1. Stop heap trace recording
2. Match allocations without corresponding frees
3. Print detailed report with backtraces
4. Free all tracker buffers

## Usage Example

### Basic Connection Leak Detection

```cpp
// In your connection handler
static int connection_count = 0;

void onNewConnection(string client_id) {
    connection_count++;

    // Start tracking on 3rd connection to measure accumulation
    if (connection_count == 3) {
        ESP_LOGI(TAG, "Starting leak tracking (connection #3)");
        leak_tracker_start(3000, 3000);
    }

    // ... create peer connection, etc ...
}

void onConnectionClosed(string client_id) {
    // ... cleanup code ...

    // Stop tracking after 3rd connection cleanup
    if (connection_count == 3) {
        // Wait for async cleanup
        vTaskDelay(pdMS_TO_TICKS(500));

        ESP_LOGI(TAG, "Stopping leak tracker");
        leak_tracker_end();
    }
}
```

### Periodic Leak Checks

```cpp
void monitor_task(void *param) {
    while (1) {
        ESP_LOGI(TAG, "Starting leak check cycle");
        leak_tracker_start(2000, 2000);

        // Let system run for a while
        vTaskDelay(pdMS_TO_TICKS(30000));  // 30 seconds

        ESP_LOGI(TAG, "Leak check results:");
        leak_tracker_end();

        // Wait between checks
        vTaskDelay(pdMS_TO_TICKS(60000));  // 1 minute
    }
}
```

## Output Format

### Leak Report Structure

```
========================================
   LEAKED ALLOCATIONS
========================================
Heap trace leak records: 18
Backtraces captured: 394

--- LEAK #1 ---
Address: 0x4809711c (PSRAM)
Size: 336000 bytes
Backtrace: (not captured - buffer full or not FreeRTOS heap)

--- LEAK #4 ---
Address: 0x4ff4fb84 (Internal)
Size: 84 bytes
Backtrace (frame pointer unwinding):
  0x4ff08bf6:0x40008f5c:0x4005cb82:0x4005cbba:0x400665b2:0x4007bf68
--- 0x4ff08bf6: xQueueCreateMutex at /home/rich/pixy3/esp-idf-v5.5.1/...
--- 0x40008f5c: pthread_mutex_init at /home/rich/pixy3/esp-idf-v5.5.1/...
--- 0x4005cb82: std::__atomic_futex_unsigned::__atomic_futex_unsigned...

========================================
   LEAK SUMMARY
========================================
Total leaks: 18 allocations, 344608 bytes (336.53 KB)
  Internal RAM: 1 allocations
  PSRAM: 17 allocations
  Backtraces found: 1 / 18

Current heap status:
  Internal free: 387 KB
  PSRAM free: 32017 KB
========================================
```

### Understanding the Output

**Address format:** `0xADDRESS (Memory type)`
- `(Internal)` - Internal RAM (MALLOC_CAP_INTERNAL)
- `(PSRAM)` - External PSRAM
- `(DMA)` - DMA-capable RAM

**Backtrace types:**
- `(frame pointer unwinding)` - Successfully captured backtrace
- `(not captured - buffer full or not FreeRTOS heap)` - No backtrace available
  - Buffer full: Increase `backtrace_records` parameter
  - Not FreeRTOS heap: Allocation from custom allocator

**Symbol resolution:**
- Shows function name, file path, and line number
- Uses ESP-IDF's address decoder
- Template function names are demangled

## Common Leak Patterns

### 1. Mutex Leaks (Before libstdc++ Patch)

```
Size: 84 bytes
--- 0x4ff08bf6: xQueueCreateMutex
--- 0x40008f5c: pthread_mutex_init
--- std::_Sp_counted_base<(__gnu_cxx::_Lock_policy)1>
```

**Cause:** shared_ptr using mutex lock policy instead of atomic
**Fix:** Apply libstdc++ atomic lock policy patch

### 2. Callback Leaks

```
Size: 84-200 bytes
--- std::function construction
--- std::_Function_base allocations
```

**Cause:** Lambda captures holding references
**Fix:** Ensure callbacks are reset on cleanup

### 3. Control Block Leaks

```
--- std::_Sp_counted_ptr_inplace
--- std::allocate_shared
```

**Cause:** Circular references in shared_ptr
**Fix:** Use weak_ptr to break cycles

## Limitations

1. **Buffer size limits:**
   - Too small: Won't capture all allocations
   - Too large: Uses more PSRAM

2. **Custom allocators:**
   - Only tracks FreeRTOS heap allocations
   - Custom allocators (like PSRAM-only) may not be captured

3. **Timing sensitivity:**
   - Must start before allocations to track
   - Must stop after full cleanup

4. **PSRAM allocations:**
   - PSRAM has 32 MB, small leaks are often acceptable
   - Focus on Internal RAM leaks (scarce resource)

## Tips for Effective Leak Detection

### 1. Measure on Nth Connection

Track on the 3rd or later connection to detect accumulation:
```cpp
if (connection_count >= 3) {
    leak_tracker_start();
}
```

### 2. Wait for Async Cleanup

Give time for background tasks to complete:
```cpp
vTaskDelay(pdMS_TO_TICKS(500));  // 500ms wait
leak_tracker_end();
```

### 3. Check Memory Before/After

```cpp
size_t before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
// ... operation ...
size_t after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
ESP_LOGI(TAG, "Memory change: %d bytes", (int)(after - before));
```

### 4. Increase Buffer for Complex Operations

For operations with many allocations:
```cpp
leak_tracker_start(5000, 5000);  // Larger buffers
```

## Integration with ESP-IDF Monitor

The ESP-IDF monitor automatically decodes addresses in the output. To get the best symbol resolution:

```bash
idf.py monitor
```

The backtrace addresses will be automatically resolved to function names and line numbers.

## Troubleshooting

### "Buffer full or not FreeRTOS heap"

**Cause:** Too many allocations or custom allocator used
**Solution:**
- Increase buffer size
- Track shorter time window
- Check if custom allocators are involved

### No Backtraces Captured

**Cause:** Frame pointer optimization disabled
**Solution:** Ensure compiler flags include frame pointer support

### High PSRAM "Leaks"

**Normal:** PSRAM leaks of a few KB are usually acceptable
**Investigate:** If PSRAM leaks grow unbounded over time

### Crashes During Tracking

**Cause:** Buffer too large or corruption
**Solution:**
- Reduce buffer sizes
- Check for stack overflows
- Ensure PSRAM is enabled

## Related Tools

- `heap_caps_print_heap_info(MALLOC_CAP_INTERNAL)` - Print heap statistics
- `heap_trace_dump()` - Raw ESP-IDF heap trace dump
- ESP-IDF GDB debugging with backtrace
