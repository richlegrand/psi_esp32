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
*Last updated: 2025-11-08 - Fixed DMA memory leak by reducing RtcpNackResponder buffer size*
