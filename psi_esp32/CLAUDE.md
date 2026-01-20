# ESP32 libdatachannel Project - Claude Context

## Project Overview
ESP32-P4 port of libdatachannel for WebRTC data channels and H.264 video streaming. The project includes a complete WebRTC H.264 streamer that reads video files from LittleFS and streams them to web browsers via WebSocket signaling.

## RESOLVED ISSUE: DMA Memory Leak - RtcpNackResponder Unbounded Growth (2025-11-08)

### Problem Summary
Application crashed after ~30 seconds of video streaming with "Mem alloc fail. size 0x00000020 caps 0x00000808". DMA heap exhausted at ~130KB/second rate, dropping from 197 KB to 3 KB before crash.

### Root Cause: RtcpNackResponder Storage Exhausting DMA
**What is RtcpNackResponder?**: Implements RTCP NACK (Negative ACKnowledgment) for packet loss recovery. Stores copies of recently sent RTP packets so they can be retransmitted if receiver detects packet loss.

**The Problem**:
- Default `maxSize=512` packets per storage (audio + video = 2 storages = 1024 packets total)
- Each stored packet: ~124 bytes of DMA (shared_ptr control blocks)
- **Total DMA usage**: 1024 × 124 = **127 KB** (75% of ESP32-P4's ~170 KB DMA!)
- Cleanup never triggered before DMA exhaustion (video reached 442 packets, audio ~70, both < 512)

### Investigation Process
1. **Traced leak to handler chain**: 91% of leak in `handler->outgoingChain()`
2. **Identified RtpPacketizer**: Confirmed packets created with refcount=1
3. **Found refcount jump 1→2**: Occurred in handler #3 of chain
4. **Used typeid()**: Discovered handler #3 was `RtcpNackResponder`, not PacingHandler
5. **Added storage instrumentation**: Confirmed size growing to 442 without cleanup

### The Fix
Changed `DefaultMaxSize` from 512 → 64 for ESP32 in `components/libdatachannel/include/rtc/rtcpnackresponder.hpp`:

```cpp
#ifdef ESP32_PORT
    // ESP32 has very limited DMA memory (~170KB total). With 512 packets × 124 bytes
    // per storage × 2 storages (audio+video) = 127KB, leaving almost no DMA for network.
    // Reduce to 64 packets (8KB per storage, 16KB total) to leave room for network stack.
    static const size_t DefaultMaxSize = 64;
#else
    static const size_t DefaultMaxSize = 512;
#endif
```

**Memory impact**:
- **Before**: 2 × 512 × 124 = 127 KB DMA (75% of total)
- **After**: 2 × 64 × 124 = 16 KB DMA (9% of total)
- **Freed**: 111 KB for network stack ✅

**Tradeoff**: 64 packets = ~200ms NACK window at 320 packets/sec. Sufficient for LAN, acceptable for internet. NACK requests were never observed during testing, suggesting this feature may be unused in typical scenarios.

### Key Insights - shared_ptr Control Block Allocation

**Critical limitation discovered**: ESP-IDF's libstdc++ may not properly use custom allocators for shared_ptr control blocks, even when using `std::allocate_shared<T, PSRAMAllocator<T>>`.

**What gets allocated**:
- `message_ptr` control block: ~124 bytes → **DMA** (despite PSRAMAllocator)
- Element objects in RtcpNackResponder: ~56 bytes → **DMA** (uses default allocator)
- unordered_map nodes: ~50 bytes → **DMA** (uses default allocator)
- Message payload: Variable → **PSRAM** ✅ (custom allocator works here)

**Why this matters**: Total per stored packet ≈ 230 bytes DMA if all allocations could be moved to PSRAM. However, shared_ptr control blocks appear locked to DMA regardless of allocator. This fundamentally limits how much can be moved to PSRAM without significant refactoring (intrusive ref-counting, raw pointers, etc.).

**Recommendation**: For memory-constrained embedded systems, minimize the number of shared_ptr copies stored in long-lived data structures. Prefer algorithms that process and discard messages immediately.

---

## RESOLVED ISSUE: libstdc++ Memory Inefficiencies for ESP32 (2025-11-16)

### Problem Summary
Two issues in ESP-IDF's libstdc++ causing significant memory waste:
1. **shared_ptr uses FreeRTOS mutex** instead of atomic operations (~88 bytes Internal RAM per shared_ptr)
2. **shared_mutex leaks memory** - destructor doesn't call `pthread_rwlock_destroy()` (~176 bytes per instance)

### Root Cause 1: Atomic Lock Policy Disabled for RISC-V

**Discovery**: GCC's `configure` script explicitly excludes RISC-V from atomic lock policy:
```c
#if defined __riscv
# error "Defaulting to mutex-based locks for ABI compatibility"
#endif
```

**Why GCC does this**: ABI compatibility for desktop RISC-V systems. If libstdc++.so (shared library) uses one lock policy and user code uses another, control block sizes mismatch at shared library boundaries. GCC plays it safe.

**Why this doesn't matter for ESP32**:
- Static linking only (no libstdc++.so)
- Single embedded application
- No binary distribution concerns
- ESP32 hardware fully supports atomic operations

**Impact**: Each `std::shared_ptr` creates a FreeRTOS mutex via `pvPortMalloc()` which always allocates from Internal RAM (hardcoded). This consumes ~88 bytes of precious Internal RAM per shared_ptr instance.

### Root Cause 2: shared_mutex Destructor Bug

**The code** in `libstdc++-v3/include/std/shared_mutex`:
```cpp
#ifdef PTHREAD_RWLOCK_INITIALIZER
// When PTHREAD_RWLOCK_INITIALIZER is defined, destructor is defaulted
// This means pthread_rwlock_destroy() is NEVER called!
~__shared_mutex_pthread() = default;  // BUG: leaks 176 bytes
#else
~__shared_mutex_pthread() {
  pthread_rwlock_destroy(&_M_rwlock);  // This path is correct
}
#endif
```

ESP-IDF defines `PTHREAD_RWLOCK_INITIALIZER` (uses lazy initialization), so destructor is defaulted and `pthread_rwlock_destroy()` never runs.

### The Solution: Patched libstdc++

Created custom build of libstdc++ at `/home/rich/pixy3/libstdcpp/` with both fixes:

**Fix 1 - Atomic lock policy** (`gcc/libstdc++-v3/configure` lines 16397-16400):
```c
/* ESP-IDF fix: Remove RISC-V exclusion for embedded use */
/* #if defined __riscv */
/* # error "Defaulting to mutex-based locks for ABI compatibility" */
/* #endif */
```

**Fix 2 - shared_mutex leak** (`gcc/libstdc++-v3/include/std/shared_mutex`):
```cpp
// Line 88: Always define destroy wrapper (outside #ifndef block)
_GLIBCXX_GTHRW(rwlock_destroy)

// Lines 166-171: Explicitly call destroy
~__shared_mutex_pthread()
{
  int __ret __attribute((__unused__)) = __glibcxx_rwlock_destroy(&_M_rwlock);
  __glibcxx_assert(__ret == 0);
}
```

**Verification**:
```bash
grep "HAVE_ATOMIC_LOCK_POLICY" /home/rich/pixy3/libstdcpp/install/include/c++/14.2.0/riscv32-esp-elf/bits/c++config.h
# Shows: #define _GLIBCXX_HAVE_ATOMIC_LOCK_POLICY 1
```

### Installation

Scripts in `/home/rich/pixy3/libstdcpp/`:
- `install_to_toolchain.sh` - Install patched library (backs up originals)
- `restore_toolchain.sh` - Restore original toolchain library
- `build.sh` - Rebuild from source if needed

```bash
cd /home/rich/pixy3/libstdcpp
./install_to_toolchain.sh

cd /home/rich/pixy3/esp32_libdatachannel
idf.py fullclean && idf.py build
```

### Expected Memory Savings
- **Per shared_ptr**: ~88 bytes Internal RAM saved (no mutex allocation)
- **Per shared_mutex**: ~176 bytes Internal RAM saved (proper cleanup)
- **RtcpNackResponder**: With 1024 shared_ptr instances, saves ~90 KB Internal RAM

### Key Insight: Header-Only Templates

Important: `std::shared_ptr` is header-only template code. The lock policy is determined by `_GLIBCXX_HAVE_ATOMIC_LOCK_POLICY` macro at **compile time**, not by what's in libstdc++.a. This is why we must install the patched `c++config.h` header, not just the library.

---

## RESOLVED ISSUE: Video Streamer Optimizations (2026-01-07)

### Non-Blocking PPA Implementation

**Problem**: PPA (Pixel Processing Accelerator) hardware scaling was operating in blocking mode, preventing CPU from doing useful work during the ~32ms downscaling operation (1280x720 → 640x360 YUV420).

**Solution**: Implemented non-blocking PPA with callback-based completion notification:

**Implementation** (`main/video_streamer.cpp`, `main/video_streamer.hpp`):
1. Added `SemaphoreHandle_t ppa_done_sem_` for completion signaling
2. Registered PPA client with `max_pending_trans_num = 3` for triple buffering
3. Added ISR-safe callback `ppaDoneCallback()` that signals semaphore
4. Changed PPA transaction mode to `PPA_TRANS_MODE_NON_BLOCKING`
5. Submit PPA → Do work in parallel → Wait for semaphore

**Key Code Pattern**:
```cpp
// Submit PPA (returns immediately)
ppa_do_scale_rotate_mirror(ppa_yuv_to_rgb_, &yuv_scale_config);

// === PPA runs in hardware while CPU does other work ===
testProcessing();  // PSRAM bandwidth test

// Wait for PPA completion
xSemaphoreTake(ppa_done_sem_, pdMS_TO_TICKS(100));
```

**Performance Measurements**:
- PPA processing time: ~32ms per frame (1280x720→640x360 YUV420)
- Frame budget at 25 fps: 40ms
- Result: ~23-24 fps actual (PPA is bottleneck)
- Opportunity: Use 32ms PPA time for parallel CV processing

**Test Processing Routine**: Added `testProcessing()` that reads 345KB from PSRAM to measure bandwidth contention between CPU and PPA hardware when both access PSRAM simultaneously.

### Task Leak Fix

**Problem**: `video_send` tasks were leaking on every connection/disconnection cycle. Each leaked task consumed:
- 16KB Internal RAM (stack)
- Task control structures
- After 6 connections: 96KB Internal RAM wasted, 6 zombie tasks

**Root Cause** (`main/video_streamer.cpp:1147`):
```cpp
// BUG: Never checks running_ flag!
while (true) {
    // Blocks forever on queue
    if (xQueueReceive(send_queue_, &frame, portMAX_DELAY) == pdTRUE) {
```

When `stopStreaming()` set `running_ = false`, the task remained blocked on `xQueueReceive()` with infinite timeout and never exited.

**Fix**:
```cpp
// Check running_ flag and use timeout
while (running_) {
    // Timeout allows periodic check of running_ flag
    if (xQueueReceive(send_queue_, &frame, pdMS_TO_TICKS(100)) == pdTRUE) {
```

Now tasks properly exit within 100ms when `running_` becomes false, calling `vTaskDelete(NULL)`.

### Frame Counter Bug Fix

**Problem**: FPS displayed as ~45 fps instead of actual ~23 fps.

**Root Cause**: `capture_frame_count_` was incremented **twice** per frame:
- Line 974: After PPA completion (capture path) ✓
- Line 1028: When getting encoder output (WRONG - duplicate)

**Fix**: Removed duplicate increment at line 1028. Counter now accurately reflects frames processed.

### PPA Performance Analysis

**Why 23 fps instead of 25 fps?**
- Target frame time: 40ms (25 fps)
- PPA processing: 32ms (hardware bottleneck)
- Other overhead: ~10ms (camera DQBUF, encoder, queueing)
- Total: ~42ms → 23.8 fps

**Options to reach 25 fps**:
1. **Skip PPA scaling**: Encode at native 1280x720 (4x pixels, but H.264 compression mitigates bandwidth increase)
2. **Use sensor binning**: Configure OV2710 to output 640x360 natively (if supported)
3. **Accept 23 fps**: Quality vs performance tradeoff

### Memory Notes

**Task Stacks**: `xTaskCreate` uses **Internal RAM** by default (16KB per task). For PSRAM stacks:
- Global option: menuconfig → FreeRTOS → "Place FreeRTOS task stacks in external memory"
- Per-task option: Use `xTaskCreateStatic` with PSRAM-allocated buffer

**Files Modified**:
- `main/video_streamer.hpp` - Added PPA semaphore, callback, test processing
- `main/video_streamer.cpp` - Non-blocking PPA implementation, task leak fix, frame counter fix

---

## CURRENT ISSUE: Callback Memory Leak Investigation (2025-11-11)

### Problem
8 KB Internal RAM leak per WebRTC connection cycle. Memory baseline: 400 KB → 392 KB after connect/disconnect/cleanup. Heap trace shows 226 leaked allocations, mostly 84-byte blocks (std::function control blocks).

### Investigation Results

**Heap trace setup**:
- 2000-record buffer allocated in PSRAM to save Internal RAM
- Trace started before connection, stopped after State::Closed
- Captures: 226 allocations, 22.4 KB total

**Observed state transitions**:
```
State: Connecting
State: Connected
State: Disconnected  (browser closes)
State: Closed        (callbacks supposedly reset here)
[~2 seconds async cleanup]
Memory: 353 KB → 392 KB (still 8 KB below baseline)
```

**Code locations examined**:
- `main/streamer_main.cpp:244-260` - onStateChange callback that erases client
- `components/libdatachannel/src/impl/peerconnection.cpp:377-420` - closeTransports() cleanup
- `components/libdatachannel/src/impl/peerconnection.cpp:1372-1382` - resetCallbacks() implementation

### Current Theory: Self-Referential Destruction

The onStateChange callback fires when State::Closed is reached. Inside that callback, `clients.erase(id)` attempts to destroy the PeerConnection. But the callback is still executing on the stack, so it can't fully destroy itself.

**Suspected sequence**:
1. State::Closed callback executes
2. Callback calls `clients.erase(id)` immediately
3. This tries to destroy PeerConnection while callback is still running
4. Callback lambda objects can't be freed until function returns
5. Result: std::function control blocks and captured data leak

**Current fix attempt**: Wrapped `clients.erase(id)` in `MainThread->dispatch()` to defer destruction until after callback completes. Testing in progress.

**Alternative hypotheses if defer doesn't work**:
- Some other component holding PeerConnection references
- Circular references in track/datachannel callbacks
- Thread pool tasks holding shared_ptr copies longer than expected

---

## PREVIOUS ISSUE: ESP-Hosted vs libdatachannel Constructor Interference (RESOLVED)

### Problem Summary
Application crashes when `startStreamer()` is called due to static initialization conflicts between libdatachannel and ESP-Hosted WiFi driver.

### Key Discovery (2025-09-28)
ESP-Hosted constructor (0x40022656) runs successfully but creates SDIO tasks that try to log before FreeRTOS scheduler starts, causing crash in `xTaskGetSchedulerState()`.

### Proposed Solution: Selective Constructor Skipping
User proposes modifying `/home/rich/pixy3/esp-idf-v5.5.1/components/esp_system/startup.c` with equality tests to skip specific constructors without changing binary layout:

```c
if (addr==0x00000001 || addr==0x00000008 || addr==0x00000015 || ...)
    skip_constructor = true;
```

After build, replace placeholder addresses with actual constructor addresses to skip.

### Constructor Address Map (Latest Build)
**Constructors to SKIP (libdatachannel/main app):**
```
0x4007d056: libdatachannel/src/impl/datachannel.cpp:395
0x4007b8c2: libdatachannel/src/impl/certificate.cpp:621
0x40079a26: libdatachannel/src/datachannel.cpp:57
0x400747c6: libdatachannel/src/impl/track.cpp:266
0x4006f6d0: libdatachannel/src/impl/sctptransport.cpp:1011
0x40067a54: libdatachannel/src/impl/peerconnection.cpp:1437
0x40054272: libdatachannel/src/impl/icetransport.cpp:949
0x40052b74: libdatachannel/src/impl/dtlstransport.cpp:1106
0x4004e7b8: libdatachannel/src/impl/dtlssrtptransport.cpp:390
0x4004cfca: libdatachannel/src/impl/init.cpp:210
0x4004a806: libdatachannel/src/track.cpp:86
0x4004a06a: libdatachannel/src/peerconnection.cpp:522
0x400471c0: libdatachannel/src/description.cpp:1425
0x40032a08: libdatachannel/src/h264rtppacketizer.cpp:98
0x40030b18: libdatachannel/src/rtppacketizer.cpp:189
0x40030050: libdatachannel/src/mediahandler.cpp:80
0x4002faa8: libdatachannel/src/rtcpsrreporter.cpp:100
0x4002da04: main/helpers.cpp:83
0x4002c936: main/stream.cpp:109
0x4002c00c: main/opusfileparser.cpp:15
0x4002bed6: main/fileparser.cpp:113
0x4002acd2: main/h264fileparser.cpp:71
0x4002a73e: main/streamer_main.cpp:367
0x400251b6: main/main.cpp:236
```

**Constructor to ALLOW (ESP-Hosted):**
```
0x40022656: esp_hosted_host_init - MUST RUN for WiFi to work!
```

---

## Build Configuration

### Dependencies (idf_component.yml)
```yaml
dependencies:
  joltwallet/littlefs: ^1.14.8
  espressif/esp_wifi_remote: "1.1.2"  # REQUIRED by ESP-Hosted (not alternative!)
  espressif/esp_hosted: "2.5.3"
  espressif/sock_utils: "*"
  espressif/esp_websocket_client: "^1.5.0"
```

### Key ESP32 Adaptations
1. **PSRAM Integration**: Custom allocators for thread stacks
2. **WiFi via ESP-Hosted**: ESP32-C6 co-processor for networking via SDIO
3. **LittleFS Storage**: Media files stored in flash partition
4. **POSIX Compatibility**: sock_utils for missing functions

## Build Commands
```bash
idf.py build          # Build project
idf.py flash monitor  # Flash and monitor
idf.py menuconfig     # Configure
```

## Network Configuration
- **WiFi SSID/Password**: Hardcoded in main.cpp (change for your network)
- **WebSocket Server**: 192.168.1.248:8000 (update main/streamer_main.cpp)
- **Media Path**: /littlefs/h264/ (H.264 files numbered 0.h264, 1.h264, etc.)

## Files Modified During Debug
- `/home/rich/pixy3/esp-idf-v5.5.1/components/esp_system/startup.c` - Constructor skipping logic
- Various libdatachannel files with singleton workarounds (may need reverting)

## Important File Locations
- `main/streamer_main.cpp` - Main streaming application with DMA monitoring
- `main/main.cpp:232` - `startStreamer()` call
- `components/libdatachannel/` - **THIS IS THE CODE BEING COMPILED** (not ~/pixy3/libdatachannel/)
- `components/libjuice/CMakeLists.txt` - libjuice build config
- `components/libsrtp/CMakeLists.txt` - SRTP component

---
*Last updated: 2026-01-07 - Added non-blocking PPA implementation, task leak fix, performance analysis*
