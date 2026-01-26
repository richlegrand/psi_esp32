#pragma once

#include <cstdint>
#include <cstddef>
#include <functional>

/**
 * Frame struct for passing image data through the CV pipeline.
 *
 * Contract:
 * 1. Input frame is borrowed - valid only during callback invocation
 * 2. Returned frame must remain valid until next callback invocation
 * 3. VideoStreamer never frees the returned pointer
 * 4. Callback can return input frame (modified in-place) or its own buffer
 */
struct Frame {
    uint8_t* data;      // Pointer to RGB888 pixel data
    int width;          // Frame width in pixels
    int height;         // Frame height in pixels
    int stride;         // Bytes per row (typically width * 3 for RGB888)

    // Calculate buffer size in bytes (assumes RGB888 format)
    size_t size() const { return static_cast<size_t>(stride) * height; }

    // Check if frame is valid
    bool isValid() const { return data != nullptr && width > 0 && height > 0; }
};

/**
 * Callback signature for CV frame processing.
 *
 * The callback receives a borrowed input frame and returns the frame to encode.
 * The returned frame can be:
 * - The same as input (modified in-place) - zero copy
 * - A different buffer owned by the callback implementer
 *
 * Thread safety: Callback runs on capture task (Internal RAM stack).
 * API calls from browser come from libdatachannel thread pool (PSRAM stack).
 * Implementers must handle synchronization if sharing state.
 */
using FrameCallback = std::function<Frame(const Frame& input)>;
