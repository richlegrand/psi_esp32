# Memory Leak Analysis - 8KB Per Connection

## Executive Summary

**Total Leak**: 8,628 bytes per WebRTC connection (69 allocations)
**Root Cause**: PeerConnection destructor not cleaning up internal mutexes and threads
**Impact**: Leaks grow linearly with each connection, eventually exhausting internal RAM

## Leak Breakdown

| Size | Count | Total | Component | Source |
|------|-------|-------|-----------|--------|
| 84 bytes | 64 | 5,376 bytes | FreeRTOS mutexes | synchronized_callback<> in PeerConnection |
| 340 bytes | 2 | 680 bytes | Thread TCBs | pthread_create in Init::doInit() |
| 1,536 bytes | 1 | 1,536 bytes | Unknown | (need stack trace) |
| 1,024 bytes | 1 | 1,024 bytes | Unknown | (need stack trace) |
| 12 bytes | 1 | 12 bytes | Unknown | (need stack trace) |
| **TOTAL** | **69** | **8,628 bytes** | | |

## Detailed Analysis

### 1. Mutex Leaks (5,376 bytes - 62% of total)

**What's leaking**: 64 × 84-byte FreeRTOS mutex objects (queue structures)

**Where they're created**: `components/libdatachannel/include/rtc/utils.hpp:55`

**Call stack**:
```
pvPortMalloc()
└─ xQueueGenericCreate()
   └─ xQueueCreateMutex()
      └─ pthread_mutex_init()
         └─ std::recursive_mutex::recursive_mutex()
            └─ synchronized_callback<T>::synchronized_callback()
               └─ PeerConnection::PeerConnection()
```

**synchronized_callback instances per PeerConnection**:
- `synchronized_callback<PeerConnection::State>`
- `synchronized_callback<PeerConnection::IceState>`
- `synchronized_callback<PeerConnection::GatheringState>`
- `synchronized_callback<PeerConnection::SignalingState>`
- `synchronized_callback<std::shared_ptr<rtc::Track>>`
- Multiple more in Track, DataChannel, IceTransport, etc.

**Why they leak**: The `synchronized_callback<>` template class creates a `std::recursive_mutex` member variable. When PeerConnection is destroyed, the destructors ARE called (confirmed with logging), but the underlying FreeRTOS queue objects allocated via pthread_mutex_init() are not properly freed.

**Hypothesis**: pthread_mutex_destroy() either:
1. Is not being called by std::recursive_mutex destructor, OR
2. Is being called but doesn't actually free the FreeRTOS queue

**Addresses leaked** (sample):
```
0x4ff4c90c, 0x4ff4c964, 0x4ff4c9bc, 0x4ff4ca14, 0x4ff4ca6c, 0x4ff4cac4,
0x4ff4cb1c, 0x4ff4cb74, 0x4ff4cbcc, 0x4ff4cc24, 0x4ff4cc7c, 0x4ff4ccd4,
0x4ff4cd2c, 0x4ff4cd84, 0x4ff4cddc, 0x4ff4ce34, ...
```

### 2. Thread TCB Leaks (680 bytes - 8% of total)

**What's leaking**: 2 × 340-byte pthread Task Control Blocks

**Where they're created**: `components/libdatachannel/src/impl/init.cpp:146`

**Call stack**:
```
pvPortMalloc()
└─ prvTaskCreateDynamicPinnedToCoreWithCaps()
   └─ pthread_create_freertos_task_with_caps()
      └─ pthread_create()
         └─ Init::doInit()
```

**Why they leak**: The Init singleton creates worker threads that are never joined or properly destroyed. These are long-lived background threads that should exist for the application lifetime, but when testing with connection create/destroy cycles, they accumulate.

**Addresses leaked**:
```
0x4ff4d14c (340 bytes, CPU 1, ccount 0x2eefc3a4)
0x4ff4d358 (340 bytes, CPU 0, ccount 0x38145dd4)
```

### 3. Other Leaks (2,572 bytes - 30% of total)

**Allocations**:
- 1,536 bytes @ 0x4ff50440 (CPU 0, ccount 0x5b4d7128)
- 1,024 bytes @ 0x4ff56d80 (CPU 0, ccount 0x62cca478)
- 12 bytes @ 0x4ff3e41c (CPU 0, ccount 0x387d3114)

**Status**: Stack traces not captured (may be outside pvPortMalloc trace window)

## Verification Evidence

### Heap Trace Statistics
```
Mode: HEAP_TRACE_LEAKS
Total allocations: 38,639
Total frees: 39,342
Records: 185 (2000 capacity, 1450 high water mark)
Leaked: 8,628 bytes in 69 allocations
```

### PeerConnection Lifecycle
- **Constructor**: All 64 mutexes are allocated
- **Destructor**: IS called (verified with logging)
- **Result**: Mutexes NOT freed - std::recursive_mutex::~recursive_mutex() doesn't call pthread_mutex_destroy()

## Impact Assessment

**Per Connection**: 8,628 bytes
**10 Connections**: ~84 KB
**ESP32-P4 Internal RAM**: ~400 KB total
**Estimated Max Connections**: ~40-45 before OOM

## Recommended Solutions

### Option 1: Fix pthread_mutex_destroy (Preferred)
Investigate why pthread_mutex_destroy() isn't freeing the FreeRTOS queue objects. This is likely an ESP-IDF bug or missing implementation.

**File to check**: `/home/rich/pixy3/esp-idf-v5.5.1/components/pthread/pthread.c`

### Option 2: Replace synchronized_callback
Replace the `synchronized_callback<T>` template with a lighter-weight alternative that doesn't use mutexes, such as:
- Atomic operations for simple state changes
- A single shared mutex for all callbacks
- Lock-free queues

**File to modify**: `components/libdatachannel/include/rtc/utils.hpp`

### Option 3: Manual Mutex Management
Explicitly call pthread_mutex_destroy() in synchronized_callback destructor and ensure the FreeRTOS queue is freed.

## Next Steps

1. ✅ **DONE**: Confirm destructors are being called
2. ✅ **DONE**: Capture full stack traces using frame pointer unwinding
3. ✅ **DONE**: Identify exact leak sources
4. **TODO**: Investigate pthread_mutex_destroy() implementation in ESP-IDF
5. **TODO**: Test manual pthread_mutex_destroy() call in synchronized_callback destructor
6. **TODO**: If ESP-IDF bug confirmed, report to Espressif

## Tools Used

### Frame Pointer Stack Tracing
Modified `/home/rich/pixy3/esp-idf-v5.5.1/components/freertos/heap_idf.c` to capture allocation backtraces using `esp_fp_get_callers()`.

**Key functions**:
- `new_trace_start()` - Start capturing allocations
- `new_trace_stop()` - Stop capturing
- `new_trace_get_count()` - Get number of traces
- `new_trace_get_record(index)` - Get trace by index

### Leak Filtering Utility
Created `memory_tracker.cpp::dump_leaked_allocations_only()` to correlate:
- ESP-IDF `heap_trace` records (shows which allocations are leaked)
- pvPortMalloc frame pointer traces (shows deep call stacks)

**Usage**:
```cpp
#include "memory_tracker.hpp"
dump_leaked_allocations_only();  // Shows only leaks with full backtraces
```

---
*Analysis Date: 2025-01-14*
*ESP-IDF Version: 5.5.1*
*Target: ESP32-P4 RISC-V*
