# ESP32 libdatachannel Memory Optimization

## Goal
Minimize internal RAM usage by libdatachannel and its dependencies to make the library usable in larger applications.

## Current Situation

### Static Allocations (from `idf.py size-components`)
Static internal RAM (DIRAM + .bss):

| Component | DIRAM | .bss | Total Static |
|-----------|-------|------|--------------|
| libdatachannel.a | 1,756 B | 1,240 B | 2,996 B (~3 KB) |
| usrsctp.a | 1,604 B | 1,504 B | 3,108 B (~3 KB) |
| libjuice.a | 1,229 B | 1,045 B | 2,274 B (~2.3 KB) |
| libsrtp.a | 1,137 B | 1,077 B | 2,214 B (~2.2 KB) |
| main.a | 736 B | 432 B | 1,168 B (~1.2 KB) |
| **TOTAL** | **6,462 B** | **5,298 B** | **~11.7 KB** |

**Managed components** use ~2.9 KB static internal RAM.

**Conclusion**: Static allocations are NOT the problem. They're minimal.

### Dynamic Allocations (Runtime)
From console logs:
- **Before connection**: ~403 KB internal RAM free
- **After connection**: ~156 KB internal RAM free
- **Used during connection**: ~247 KB

**This is the real issue!** Dynamic allocations are consuming ~247 KB of internal RAM.

---

## Phase 1: Detailed Accounting

### Components with PSRAM Redirection ✅
Currently using `esp32_malloc_redirect.h`:
1. **libdatachannel** ✅
2. **libjuice** ✅
3. **usrsctp** ✅

These components allocate from PSRAM via `heap_caps_malloc(size, MALLOC_CAP_SPIRAM)`.

### Components WITHOUT PSRAM Redirection ❌
1. **libsrtp** ❌ - Uses default malloc (internal RAM!)
2. **main application code** ❌ - Uses default malloc
3. **managed components** ❌ - esp_hosted, websocket_client, etc.
4. **C++ STL allocations** ❌ - std::string, std::vector, etc. may use internal RAM

### Thread Stacks
- `esp32_configure_pthread_psram()` is called in main.cpp
- Configures pthread stacks to use PSRAM (32 KB default)
- **usrsctp creates multiple threads** - these should be in PSRAM ✅
- **libdatachannel threads** - should be in PSRAM ✅
- But: Any tasks created with `xTaskCreate()` NOT using pthread will use internal RAM ❌

### Likely Internal RAM Consumers

1. **libsrtp allocations** (~5-10 KB estimated)
   - SRTP keys, contexts, replay protection windows
   - Currently using default malloc → internal RAM

2. **C++ STL containers** (20-50 KB estimated)
   - std::string, std::vector, std::map in libdatachannel
   - Unless explicitly using custom allocators, these use internal RAM

3. **mbedTLS** (30-50 KB estimated)
   - SSL/TLS contexts, certificates, keys
   - Large temporary buffers during handshake

4. **WebSocket/HTTP buffers** (10-20 KB)
   - esp_websocket_client buffers
   - HTTP parser buffers

5. **ICE/STUN/TURN** (20-30 KB)
   - libjuice candidate gathering
   - STUN transaction buffers
   - Connectivity check state

6. **SCTP association** (30-50 KB)
   - usrsctp association state
   - Send/receive buffers
   - Stream management

7. **RTP/RTCP** (20-40 KB)
   - Packetizer buffers
   - NACK responder state
   - SR/RR report tracking

---

## Phase 2: Easiest Changes with Largest Impact

### Priority 1: Add PSRAM Redirection to libsrtp (Quick Win)
**Effort**: Low (5 min)
**Impact**: Medium (5-10 KB saved)
**Risk**: Low

Add to `/components/libsrtp/CMakeLists.txt`:
```cmake
target_compile_options(${COMPONENT_LIB} PRIVATE
    -include ${CMAKE_CURRENT_LIST_DIR}/../libdatachannel/include/esp32_malloc_redirect.h
)
```

### Priority 2: Add Custom STL Allocator (Medium Win)
**Effort**: Medium (1-2 hours)
**Impact**: High (20-50 KB saved)
**Risk**: Medium (requires testing)

Create PSRAM-based allocator for C++ STL containers:
- std::string → std::basic_string<char, std::char_traits<char>, PsramAllocator<char>>
- std::vector<T> → std::vector<T, PsramAllocator<T>>
- std::map, std::list, etc.

**Files affected**: headers defining public APIs using STL containers.

### Priority 3: Configure mbedTLS for PSRAM (Big Win)
**Effort**: Medium (2 hours)
**Impact**: High (30-50 KB saved)
**Risk**: Medium

mbedTLS supports custom allocators:
```c
mbedtls_platform_set_calloc_free(psram_calloc, psram_free);
```

Need to call this before any mbedTLS initialization.

### Priority 4: Increase FreeRTOS Heap in PSRAM
**Effort**: Low (config change)
**Impact**: Indirect (allows more internal RAM for other uses)
**Risk**: Low

In `sdkconfig`:
```
CONFIG_FREERTOS_HEAP=0  # Disable FreeRTOS heap in internal RAM
```
Use only `heap_caps_malloc()` everywhere.

### Priority 5: Audit and Fix Direct malloc() Calls
**Effort**: High (4-6 hours)
**Impact**: High (varies, could be 50+ KB)
**Risk**: High (requires extensive testing)

Search all source files for:
- `malloc()`, `calloc()`, `realloc()` not redirected
- `new` / `new[]` operators
- Third-party library allocations

---

## Phase 3: Implementation Plan

### Step 1: Add Memory Tracking (Measurement)
**Files created:**
- `main/memory_tracker.cpp`
- `main/memory_tracker.hpp`

**Usage:**
```cpp
#include "memory_tracker.hpp"

// At startup
MemorySnapshot baseline = capture_memory_snapshot();
print_detailed_memory_stats("Startup");

// After each major operation
print_memory_delta("After connection", baseline);
print_task_stack_usage();
analyze_heap_fragmentation();
```

Add calls in `main.cpp` and `streamer_main.cpp` at key points:
1. After WiFi init
2. After WebRTC init
3. After connection established
4. During streaming

This will show exactly where internal RAM is being consumed.

### Step 2: Quick Wins (Priority 1 & 4)
1. Add PSRAM redirect to libsrtp
2. Build and test
3. Measure memory usage difference
4. If successful, commit

### Step 3: Medium Wins (Priority 2 & 3)
1. Implement PSRAM STL allocator
2. Configure mbedTLS for PSRAM
3. Build and test extensively
4. Measure memory usage
5. Commit if stable

### Step 4: Deep Audit (Priority 5)
1. Enable heap tracing: `CONFIG_HEAP_TRACING=y`
2. Capture heap trace during connection
3. Analyze with `idf.py heap-trace`
4. Identify specific allocation sites
5. Fix one by one, testing after each

---

## Measurement Tools

### Built-in ESP-IDF Tools
```bash
# Static analysis
idf.py size
idf.py size-components

# Runtime analysis
idf.py monitor  # Watch console output
```

### Heap Tracing (Advanced)
In code:
```cpp
#include <esp_heap_trace.h>

// Initialize tracing
heap_trace_init_standalone(trace_records, NUM_RECORDS);

// Start tracing
heap_trace_start(HEAP_TRACE_ALL);

// ... do operations ...

// Stop and dump
heap_trace_stop();
heap_trace_dump();
```

### Our Custom Memory Tracker
See `memory_tracker.cpp` - provides detailed snapshots and deltas.

---

## Expected Results

After implementing Priority 1-3:
- **Internal RAM saved**: 55-110 KB (conservative estimate)
- **Remaining free after connection**: ~211-266 KB (vs current 156 KB)
- **Improvement**: 35-70% more internal RAM available

This would make the library much more usable in larger applications.

---

## Notes

- **DO NOT** redirect lwIP allocations to PSRAM - lwIP MUST use DMA-capable internal RAM for zero-copy operations
- **DO NOT** move interrupt handlers or ISR data structures to PSRAM
- **TEST THOROUGHLY** after each change - PSRAM is slower than internal RAM (~80 MHz vs 360 MHz)
- Watch for performance regressions in latency-sensitive code paths
- Some allocations MUST remain in internal RAM for hardware DMA access

---

*Last updated: 2025-11-09 - Phase 1 analysis complete*
