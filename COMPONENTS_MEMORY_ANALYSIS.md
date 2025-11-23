# Components Memory Analysis - Internal RAM Usage

## Focus
Analyze internal RAM usage ONLY for code in `/components` directory:
- `libdatachannel/`
- `libjuice/`
- `usrsctp/`
- `libsrtp/`
- `plog/`

Ignore `main/` (application demo) and `managed_components/` (third-party, hard to modify).

---

## The Critical Questions

### Q1: Baseline Memory - Where does the first ~368 KB go?
**Observed**: ~400 KB free at startup (out of 768 KB total)
**Question**: What consumes ~368 KB BEFORE any WebRTC connection?

### Q2: Connection Memory - Where does the next ~247 KB go?
**Observed**: ~156 KB free after connection (was ~400 KB before)
**Question**: What consumes ~247 KB during connection setup?

---

## Analysis Plan

### Step 1: Instrument Startup Sequence

Added memory tracking at key points in `main.cpp`:
1. After NVS init (before any component/ code runs)
2. After LittleFS (still before component/ code)
3. After WiFi (managed component)
4. **Before `rtc::InitLogger()`** ← First component/ code
5. **After `rtc::InitLogger()`**
6. **After `rtc::StartNetworking()`**
7. During connection setup

**Build and run to get these measurements.**

### Step 2: Identify Component Allocations

What could our components allocate at startup?

#### libdatachannel
- **Logger initialization**: plog creates Logger, Appenders
  - Estimated: < 10 KB
- **Thread pool**: `StartNetworking()` spawns threads
  - Threads use PSRAM stacks (32 KB each, configured in psram_allocator.cpp)
  - Thread control blocks: ~1 KB per thread in internal RAM
  - Hardware concurrency on ESP32-P4: 2 cores → 2 threads
  - Estimated: ~2 KB internal RAM
- **Static C++ objects**:
  - Singletons (ThreadPool, Init, etc.)
  - Estimated: ~5 KB
- **STL containers in static/global scope**:
  - std::map, std::vector in singletons
  - **Problem**: These use default allocator → internal RAM!
  - Estimated: 10-50 KB

#### usrsctp
- **Global SCTP state**:
  - CRC tables (const, in flash)
  - SCTP base info structures
  - Estimated: ~5 KB
- **Pre-allocated buffers**: Need to check if any
- **Thread creation**: usrsctp creates a receive thread
  - Using pthread → PSRAM stack ✅
  - Control block: ~1 KB internal RAM

#### libjuice
- **ICE agent state**: Allocated when agent created (during connection, not startup)
- **Static initialization**: Minimal
- **Thread creation**: Background threads for connectivity checks
  - But created during connection, not at startup

#### libsrtp
- **Crypto state**: Initialized during DTLS handshake (not at startup)
- **Key derivation buffers**: Runtime, not startup
- **Static initialization**: Minimal

#### plog
- **Logger instance**: Created by `InitLogger()`
- **Appenders, formatters**: Small objects
- Estimated: < 5 KB

---

## Step 3: Components Using PSRAM Redirection

Currently configured in CMakeLists.txt:

✅ **libdatachannel**: `-include esp32_malloc_redirect.h`
  - malloc/free redirected to PSRAM
  - BUT: C++ STL still uses internal RAM!

✅ **libjuice**: `-include esp32_malloc_redirect.h`
  - malloc/free redirected to PSRAM

✅ **usrsctp**: `-include esp32_malloc_redirect.h`
  - malloc/free redirected to PSRAM

❌ **libsrtp**: NO redirection
  - All allocations go to internal RAM!

❌ **plog**: NO redirection (header-only library)
  - Allocations via libdatachannel's malloc (PSRAM ✅)

---

## Step 4: The C++ STL Problem

**Critical Issue**: C++ STL containers use internal RAM even with malloc redirection!

Example:
```cpp
// In libdatachannel singletons:
static std::map<int, Handler> handlers;  // Uses std::allocator → internal RAM!
static std::vector<Task> tasks;          // Uses std::allocator → internal RAM!
```

The `malloc` macro redirection doesn't affect C++ `operator new` or STL allocators!

### Affected Containers (Estimate)
Looking at libdatachannel source:
- `ThreadPool`: std::deque<task>, std::vector<thread>
- `Init`: std::vector, std::map
- `PeerConnection`: std::map, std::vector, std::unordered_map
- `IceTransport`: std::vector
- `SctpTransport`: std::map
- `Track`: std::vector

**Estimated total**: 20-100 KB depending on connection state

---

## Step 5: Quick Wins

### Priority 1: Add libsrtp PSRAM Redirection
**File**: `components/libsrtp/CMakeLists.txt`

Add:
```cmake
target_compile_options(${COMPONENT_LIB} PRIVATE
    -include ${CMAKE_CURRENT_LIST_DIR}/../libdatachannel/include/esp32_malloc_redirect.h
)
```

**Expected savings**: 5-15 KB

### Priority 2: Override C++ operator new/delete
**File**: `components/libdatachannel/psram_allocator.cpp`

Add to bottom:
```cpp
// Override global operator new to use PSRAM
void* operator new(size_t size) {
    return esp32_psram_malloc(size);
}

void* operator new[](size_t size) {
    return esp32_psram_malloc(size);
}

void operator delete(void* ptr) noexcept {
    esp32_psram_free(ptr);
}

void operator delete[](void* ptr) noexcept {
    esp32_psram_free(ptr);
}

// C++14 sized deallocation
void operator delete(void* ptr, size_t) noexcept {
    esp32_psram_free(ptr);
}

void operator delete[](void* ptr, size_t) noexcept {
    esp32_psram_free(ptr);
}
```

**Expected savings**: 20-100 KB (STL containers will use PSRAM!)
**Risk**: Low - this is standard practice for custom allocators

---

## Step 6: Deeper Analysis (If Quick Wins Insufficient)

### Enable Heap Tracing
In `sdkconfig`:
```
CONFIG_HEAP_TRACING=y
CONFIG_HEAP_TRACING_STACK_DEPTH=10
```

In code:
```cpp
#include <esp_heap_trace.h>

#define NUM_RECORDS 200
static heap_trace_record_t trace_records[NUM_RECORDS];

void start_tracing() {
    heap_trace_init_standalone(trace_records, NUM_RECORDS);
    heap_trace_start(HEAP_TRACE_ALL);
}

void stop_and_dump_trace() {
    heap_trace_stop();
    heap_trace_dump();
}
```

This will show:
- Every allocation site (file:line)
- Size of each allocation
- Call stack

### Analyze with objdump
Check actual .bss section sizes:
```bash
riscv32-esp-elf-objdump -h build/esp-idf/libdatachannel/liblibdatachannel.a
riscv32-esp-elf-nm -S -l build/esp-idf/libdatachannel/liblibdatachannel.a | grep -i bss
```

---

## Next Steps

1. **Build with current instrumentation** to see memory at each checkpoint
2. **Implement Priority 1** (libsrtp PSRAM) - 5 minutes
3. **Test and measure** - does it help?
4. **Implement Priority 2** (operator new) - 10 minutes
5. **Test and measure** - should see significant improvement
6. If still not enough, enable heap tracing for detailed analysis

---

## Expected Results

After Priority 1 + 2:
- **Baseline (startup)**: +25-115 KB more internal RAM free
- **After connection**: +25-115 KB more internal RAM free
- **Total improvement**: Could go from 400 KB → 500+ KB free at startup

---

*Focus: Components directory only. Ignore application and managed components.*
