# ESP32 libdatachannel Project - Claude Context

## Project Overview
ESP32-P4 port of libdatachannel for WebRTC data channels and H.264 video streaming. The project includes a complete WebRTC H.264 streamer that reads video files from LittleFS and streams them to web browsers via WebSocket signaling.

---

## Architecture: PSI Signaling and WebRTC DataChannel Proxy

### Overview

This project uses a unique architecture where the ESP32 acts as a "web server" without exposing any ports. All communication is tunneled through WebRTC, with a signaling server facilitating the initial connection.

**Key Components**:
- **Signaling Server**: `~/ringtail/signaling/psi/psisignaling.py` - Routes WebSocket messages between ESP32 and browsers
- **Browser Client**: `~/ringtail/signaling/psi/__nf__client.js` - WebRTC client that connects to ESP32
- **Service Worker**: `~/ringtail/signaling/psi/__nf__sw.js` - Proxies HTTP requests over DataChannel
- **ESP32 WebRTC Handler**: `main/httpd_server.cpp` - Handles signaling and serves HTTP over DataChannel
- **ESP32 HTTP Handlers**: `main/httpd_test.c` - ESP-IDF style HTTP handlers (work over DataChannel)

### Connection Flow

```
┌─────────┐     WebSocket      ┌──────────────────┐     WebSocket      ┌─────────┐
│ Browser │◄──────────────────►│ Signaling Server │◄──────────────────►│  ESP32  │
└─────────┘  /ws/client/<uid>  │ psi.vizycam.com  │  /ws/device/<uid>  └─────────┘
     │                         └──────────────────┘                          │
     │                                                                       │
     │                         WebRTC (P2P)                                  │
     └───────────────────────────────────────────────────────────────────────┘
                          DataChannel + Video Track
```

1. **ESP32 Registers**: Connects to signaling server via WebSocket at `/ws/device/<uid>` (e.g., UID "0123456789")
2. **Browser Connects**: User visits `https://psi.vizycam.com/0123456789`, loads `__nf__client.js`
3. **Browser Requests Connection**: Sends `{type: "request", uid: "..."}` via WebSocket
4. **Signaling Server Routes**: Forwards request to ESP32

### WebRTC Handshake (ESP32 as Offerer)

**Why ESP32 creates the offer**: The ESP32 needs to add the video track before creating the offer. If the browser offered first, renegotiation would be required after ESP32 adds its track—this is simpler.

1. ESP32 receives "request" → creates PeerConnection → adds video track → creates **offer**
2. Signaling server forwards offer to browser
3. Browser receives offer → sets remote description → creates **answer**
4. Signaling server forwards answer to ESP32
5. Both sides exchange **ICE candidates** via signaling server
6. **DataChannel opens** + **Video track ready**

### Service Worker HTTP Proxy

The service worker (`__nf__sw.js`) intercepts fetch requests and routes them through the WebRTC DataChannel, making the ESP32 appear as a normal web server to page JavaScript.

```javascript
// Service worker intercepts fetch to /static/* paths
self.addEventListener('fetch', event => {
    if (url.pathname.startsWith("/static/")) {
        event.respondWith(proxyViaDataChannel(request));
    }
});
```

**Flow**:
1. Page requests `/static/images/image1.jpg`
2. Service worker intercepts → sends request over DataChannel
3. ESP32 receives → `image_handler()` reads file from LittleFS → sends response
4. Service worker receives → returns as normal fetch Response
5. Page receives image (transparent to application code)

### SWSP Protocol (Simple WebRTC Service Protocol)

Binary frame format for HTTP-over-DataChannel:

```
┌────────────┬────────┬────────┬─────────────────┐
│ stream_id  │ flags  │ length │    payload      │
│  4 bytes   │2 bytes │2 bytes │  N bytes        │
│  (LE u32)  │(LE u16)│(LE u16)│                 │
└────────────┴────────┴────────┴─────────────────┘
```

**Flags**:
- `FLAG_SYN = 0x0001` - Metadata frame (JSON headers)
- `FLAG_FIN = 0x0004` - Final frame in stream

**Request** (Browser → ESP32):
```json
{"method": "GET", "pathname": "/static/images/image1.jpg"}
```

**Response** (ESP32 → Browser):
- First frame (FLAG_SYN): `{"status": 200, "headers": {"Content-Type": "image/jpeg"}}`
- Middle frames: Binary data chunks
- Last frame (FLAG_FIN): Final data chunk

### ESP32 HTTP Handlers

`main/httpd_test.c` implements ESP-IDF compatible HTTP handlers that work over DataChannel:

```c
// Root handler serves HTML with video element
static esp_err_t root_handler(httpd_req_t *req) {
    const char* html = "... <video autoplay> ...";
    httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Image handler streams files from LittleFS
static esp_err_t image_handler(httpd_req_t *req) {
    // Must allocate buffer in Internal RAM (PSRAM stack can't do flash I/O)
    char *chunk = heap_caps_malloc(CHUNK_SIZE, MALLOC_CAP_INTERNAL);
    // Stream file in chunks...
}
```

### Video Streaming

The HTML page served by `root_handler()` includes a `<video>` element. Video is streamed via WebRTC **media track** (not DataChannel):

- **DataChannel**: HTTP requests (HTML, images, API calls)
- **Media Track**: H.264 video stream from `VideoStreamer`

### File Locations

| Component | Path |
|-----------|------|
| Signaling Server | `~/ringtail/signaling/psi/psisignaling.py` |
| Browser Client JS | `~/ringtail/signaling/psi/__nf__client.js` |
| Service Worker | `~/ringtail/signaling/psi/__nf__sw.js` |
| ESP32 WebRTC/Signaling | `main/httpd_server.cpp` |
| ESP32 HTTP Handlers | `main/httpd_test.c` |
| ESP32 Video Streamer | `main/video_streamer.cpp` |

---

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

## RESOLVED ISSUE: Runtime Video Source Auto-Detection (2026-01-24)

### Problem Summary
VideoStreamer used compile-time defines (`VIDEO_SOURCE_CAMERA` / `VIDEO_SOURCE_JPEG_FILES`) requiring separate builds for camera vs JPEG playback modes. Need automatic runtime detection while avoiding memory allocation failures and flash I/O safety violations.

### Critical Constraints Discovered

**1. Flash I/O Safety with PSRAM Stacks**
- **Cannot read from flash (LittleFS) when running on PSRAM stack** - ESP32 hardware restriction
- `startStreaming()` runs from libdatachannel ThreadPool with PSRAM stack → **NO flash I/O allowed**
- `captureLoop()` runs in capture task with Internal RAM stack → **Safe for flash I/O**

**2. Memory Allocation Order**
- H.264 encoder allocates **353 KB** with `MALLOC_CAP_CACHE_ALIGNED | MALLOC_CAP_INTERNAL` during init
- JPEG mode allocates **3+ MB** in PSRAM (decode buffers, file buffers)
- **If JPEG init happens before encoder init**, encoder's 353KB allocation fails (PSRAM fragmentation)

**3. xTaskCreate Stack Allocation**
- `xTaskCreate` uses **Internal RAM** by default for task stacks (16KB per task)
- This makes capture task safe for flash I/O operations
- For PSRAM stacks: Use menuconfig global option or `xTaskCreateStatic` with PSRAM buffer

### The Solution: Lazy Initialization Pattern

**Detection Phase** (Constructor):
```cpp
VideoSource VideoStreamer::detectVideoSource() {
    // Lightweight - just checks directory and scans for .jpg files
    DIR* dir = opendir("/littlefs/frames");
    if (!dir) return VideoSource::CAMERA;

    // Check for JPEG files...
    if (has_jpegs) return VideoSource::JPEG_FILES;
    return VideoSource::CAMERA;
}
```

**Initialization Order** (Critical for Memory):
1. **Constructor**: Detection only (lightweight, no large allocations)
2. **startStreaming()**: Encoder init (353 KB) → PPA init (small DMA) → NO JPEG init
3. **captureLoop()**: Lazy JPEG init (3+ MB PSRAM) - only if JPEG mode, only on first frame

**Lazy JPEG Init** (in Internal RAM stack task):
```cpp
void VideoStreamer::captureLoop() {
    // Lazy init for JPEG mode - safe for flash I/O (Internal RAM stack)
    if (video_source_ == VideoSource::JPEG_FILES && !jpeg_reader_) {
        if (!initJpegMode()) {
            running_ = false;
            return;
        }
    }

    while (running_) {
        if (video_source_ == VideoSource::CAMERA) {
            // Camera path...
        } else {  // VideoSource::JPEG_FILES
            // JPEG path (flash I/O safe here)...
        }
    }
}
```

### Key Benefits

1. **Single binary**: One build serves both camera and JPEG modes
2. **Automatic detection**: Checks `/littlefs/frames/` for JPEG files at startup
3. **Memory safe**: Preserves encoder-first allocation order
4. **Flash I/O safe**: JPEG init in Internal RAM stack task
5. **Runtime flexibility**: Same binary can switch modes with different flash images

### Implementation Details

**Files Modified**:
- `main/video_streamer.hpp` - Added VideoSource enum, removed all `#ifdef VIDEO_SOURCE_*` guards
- `main/video_streamer.cpp` - Runtime detection, lazy init, conditional cleanup
- `CMakeLists.txt` - Removed compile-time VIDEO_SOURCE selection
- `main/CMakeLists.txt` - JPEG sources always compiled

**Detection Logic**:
- If `/littlefs/frames/` exists and contains `.jpg` files → JPEG mode
- Otherwise → Camera mode

**Runtime Branching**: All `#ifdef VIDEO_SOURCE_*` converted to `if (video_source_ == ...)`

### Critical Insights

**Why lazy initialization is essential**:
1. `startStreaming()` called from ThreadPool (PSRAM stack) - cannot do flash I/O
2. Encoder must initialize before JPEG buffers (memory order)
3. `captureLoop()` has Internal RAM stack - safe for `opendir`, `readdir`, file reads
4. Pattern matches existing working code (prevents regressions)

**Flash I/O restriction applies to**:
- `opendir()`, `readdir()`, `closedir()` - directory traversal
- `open()`, `read()`, `close()` - file I/O
- Any LittleFS operations from PSRAM stack → crash or corruption

**Safe locations for flash I/O**:
- app_main task (Internal RAM stack)
- Tasks created with `xTaskCreate` (Internal RAM stack by default)
- Tasks created with `xTaskCreateStatic` using Internal RAM buffer

### Also Implemented: Runtime CV Pipeline Toggle

Previously CV pipeline was compile-time (`#define CV_PIPELINE`). Now controlled by constructor parameter:

```cpp
VideoStreamer(uint32_t output_width, uint32_t output_height,
              uint32_t fps = 25,
              bool enable_cv = false);  // Runtime CV toggle
```

**CV Pipeline Modes**:
- `enable_cv = false`: Direct YUV→YUV scaling (default)
- `enable_cv = true`: YUV→RGB→CV processing→RGB→YUV (for computer vision)

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

**IMPORTANT: Always source the ESP-IDF environment first!**
```bash
source ~/pixy3/esp-idf-v5.5.1/export.sh   # REQUIRED before any idf.py command!

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

## PLANNED: HueTrack Color Tracking Port

### Overview

Port the color tracking algorithm from `~/pixy3/tracking/huetrack.py` to run on ESP32 within the video pipeline. The algorithm learns color statistics from a user-selected region and tracks objects of that color using contour detection.

### Source Algorithm (huetrack.py)

**Color Model**:
- Uses Pixy-style UV color space: `u = (R-G)/(R+G+B)`, `v = (B-G)/(R+G+B)`
- Stores mean and covariance matrix for selected color
- Classification via Mahalanobis distance thresholding

**Learning**:
- `iterative_teach()` - Refines color model iteratively from initial ROI
- Uses morphological operations and contour finding to isolate target

**Tracking**:
- `Tracker` class manages object correspondence across frames
- State machine: INTRO → TRACKING → LOST
- Adaptive mean update with drift limiting

### Key Challenge: Frame Synchronization

When user selects a region in the browser, we need the ESP32 to extract pixels from the **exact same frame** the user sees. Solution: **Freeze Mode**.

### Freeze Mode Design

```
Browser                                    ESP32
   │                                         │
   │──── POST /api/freeze ──────────────────>│
   │                                         │ 1. Capture frame into buffer
   │<─── OK ─────────────────────────────────│ 2. Enter freeze mode
   │                                         │    (send same frame repeatedly)
   │                                         │
   │ Video shows frozen frame                │
   │ User draws box directly on <video>      │
   │                                         │
   │──── POST /api/teach {x,y,w,h} ─────────>│
   │                                         │ 3. Learn from stored RGB buffer
   │<─── OK ─────────────────────────────────│ 4. Exit freeze mode
   │                                         │
   │ Video resumes live                      │
```

**VideoStreamer Changes**:
- Add `freeze_requested_` and `frozen_` atomic flags
- When frozen: skip camera capture, resubmit same `yuv_output_buffer_` to encoder
- Keep `rgb_buffer_` intact for color learning (requires CV pipeline enabled)

### Implementation Phases

**Phase 1: Freeze Mode**
- Add freeze state to VideoStreamer (`requestFreeze()`, `isFrozen()`, `unfreeze()`)
- Modify `captureLoop()` to handle frozen state
- Add `/api/freeze` HTTP endpoint

**Phase 2: Browser Region Selection UI**
- Add JavaScript to HTML for mouse drag rectangle selection on `<video>`
- "Freeze" button → POST /api/freeze
- Visual rectangle overlay during selection
- "Teach" button → POST /api/teach with region coordinates

**Phase 3: Basic Color Learning**
- Create `huetrack.hpp/cpp` with `ColorModel` struct
- Implement UV extraction from RGB buffer region
- Compute mean and covariance
- `/api/teach` endpoint triggers learning

**Phase 4: Pixel Classification**
- Implement Mahalanobis distance classification
- Draw colored overlay on matching pixels in `processCVFrame()`

**Phase 5: Contour Detection**
- Implement morphological operations (erode/dilate with 3x3 kernel)
- Implement connected components or contour tracing
- Compute blob centroid and area

**Phase 6: Object Tracking**
- Port `Tracker` class state machine
- Object correspondence (match detections to existing objects by distance)
- Handle INTRO validation, LOST timeout

**Phase 7: Mean Adaptation**
- IIR filter for gradual mean updates
- Drift limiting from original model

**Phase 8: Browser Feedback**
- Option A: Draw tracking results into RGB frame (burns into video)
- Option B: Send tracking data via DataChannel for browser overlay

### Memory Considerations

- RGB buffer already allocated when CV pipeline enabled: `output_width * output_height * 3`
- Color model: ~100 bytes (mean, covariance, inverse covariance)
- Per-object state: ~200 bytes (centroid, velocity, state, contours reference)
- Contour storage: Variable, depends on object complexity

### Prerequisites

**Before implementing**, clean up `video_streamer.cpp`:
- `captureLoop()` is complex and has structural issues (duplicate camera buffer returns)
- Refactor for clarity before adding freeze mode logic

---

## PLANNED: Browser-Side Overlay Synchronization

### Overview

Render tracking overlays (rectangles, crosshairs) on the browser side synchronized with video, instead of burning them into the video stream. This allows browser-controlled visualization and avoids permanent modification of the video.

### Timestamp Correlation

ESP32's relative PTS and browser's `mediaTime` from `requestVideoFrameCallback` should align:

| ESP32 | Browser |
|-------|---------|
| `(esp_timer_get_time() - video_start_pts_) / 1000000.0` | `metadata.mediaTime` |

Both measure seconds since stream start.

### Architecture

```
VideoStreamer::processFrameCV()
    │
    │ current_frame_pts_ = (now - start) / 1e6
    │
    └─► frame_callback_(input)              // ColorTracker::onFrame()
            │
            │ Detects contours
            │ Calls sendOverlay({objects:[...]})
            │
            └─► VideoStreamer::sendOverlay()
                    │
                    │ Adds current_frame_pts_ to JSON
                    │ Sends via DataChannel
                    ▼
                Browser receives {type:"track", pts:1.234, objects:[...]}
```

### ESP32 Implementation

**VideoStreamer** (owns PTS, sends overlay data):
```cpp
// video_streamer.hpp
void sendOverlay(const std::string& json_data);
double current_frame_pts_ = 0;

// video_streamer.cpp
void VideoStreamer::sendOverlay(const std::string& json_data) {
    char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"type\":\"track\",\"pts\":%.3f,\"data\":%s}",
        current_frame_pts_, json_data.c_str());

    for (auto& [client_id, dc] : data_channels_) {
        if (dc && dc->isOpen()) dc->send(buf);
    }
}

// In processFrameCV(), before calling callback:
current_frame_pts_ = (esp_timer_get_time() - video_start_pts_) / 1000000.0;
```

**ColorTracker** (produces overlay data, no timestamp awareness):
```cpp
// color_tracker.hpp
void setOverlaySender(std::function<void(const std::string&)> fn);

// color_tracker.cpp - in onFrame() after tracking:
if (tracking_enabled_ && !contours_.empty() && send_overlay_) {
    std::string json = formatContoursJson(contours_);
    send_overlay_(json);
}
```

### Browser Implementation

**Canvas overlay with `requestVideoFrameCallback`**:
```javascript
const trackingBuffer = [];
const MAX_BUFFER = 30;

// Receive tracking data
dataChannel.onmessage = (e) => {
    const msg = JSON.parse(e.data);
    if (msg.type === 'track') {
        trackingBuffer.push(msg);
        if (trackingBuffer.length > MAX_BUFFER) trackingBuffer.shift();
    }
};

// Find data matching video time
function findTrackingData(mediaTimeMs) {
    let best = null;
    for (const t of trackingBuffer) {
        if (t.pts * 1000 <= mediaTimeMs + 100) {  // 100ms tolerance
            if (!best || t.pts > best.pts) best = t;
        }
    }
    return best;
}

// Render synchronized with video frames
video.requestVideoFrameCallback(function onFrame(now, metadata) {
    const tracking = findTrackingData(metadata.mediaTime * 1000);

    ctx.clearRect(0, 0, canvas.width, canvas.height);
    if (tracking?.data?.objects) {
        ctx.strokeStyle = 'lime';
        for (const obj of tracking.data.objects) {
            ctx.strokeRect(
                obj.x * canvas.width, obj.y * canvas.height,
                obj.w * canvas.width, obj.h * canvas.height
            );
        }
    }

    video.requestVideoFrameCallback(onFrame);
});
```

### Browser Support

`requestVideoFrameCallback` has ~95% global support (Chrome 83+, Firefox 132+, Safari 15.4+).

Fallback for older browsers:
```javascript
if (!('requestVideoFrameCallback' in HTMLVideoElement.prototype)) {
    // Use requestAnimationFrame + video.currentTime (less accurate)
}
```

### Files to Modify

| File | Changes |
|------|---------|
| `main/video_streamer.hpp` | Add `sendOverlay()`, `current_frame_pts_` |
| `main/video_streamer.cpp` | Store PTS before callback, implement `sendOverlay()` |
| `main/color_tracker.hpp` | Add `setOverlaySender()` callback |
| `main/color_tracker.cpp` | Call overlay sender after tracking |
| `main/httpd_server.cpp` | Wire up overlay sender |
| `media_files/static/app.js` | Add canvas overlay, tracking buffer, frame callback |

---
*Last updated: 2026-01-26 - Added browser-side overlay synchronization plan*
