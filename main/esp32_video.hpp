/**
 * ESP32-P4 H.264 Video Capture and Encoding
 *
 * Wraps ESP32 V4L2 camera capture and hardware H.264 encoder
 * for WebRTC streaming over libdatachannel.
 */

#ifndef ESP32_VIDEO_HPP
#define ESP32_VIDEO_HPP

#include <functional>
#include <memory>
#include <atomic>
#include <cstdint>

extern "C" {
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
}

class ESP32Video {
public:
    // H.264 frame callback
    // data: Pointer to H.264 frame data (Annex-B format with start codes)
    // size: Frame size in bytes
    // timestamp_us: Frame timestamp in microseconds
    // keyframe: True if this is a keyframe (IDR)
    using FrameCallback = std::function<void(const uint8_t* data, size_t size, uint64_t timestamp_us, bool keyframe)>;

    ESP32Video(uint32_t width = 640, uint32_t height = 480, uint32_t fps = 30);
    ~ESP32Video();

    // Initialize camera and encoder
    bool open();

    // Start capture/encode loop in background task
    // Calls frameCallback for each encoded frame
    bool start(FrameCallback callback);

    // Stop capture/encode
    void stop();

    // Close devices
    void close();

    // Force keyframe on next encode
    void forceKeyframe();

    uint32_t getWidth() const { return width_; }
    uint32_t getHeight() const { return height_; }
    uint32_t getFPS() const { return fps_; }

private:
    uint32_t width_;
    uint32_t height_;
    uint32_t fps_;

    // Device file descriptors
    int cap_fd_;       // Camera capture device
    int m2m_fd_;       // H.264 encoder device

    // Buffers
    static constexpr int CAM_BUFFER_COUNT = 4;
    static constexpr int ENCODER_OUTPUT_BUFFERS = 3;
    uint8_t* cap_buffer_[CAM_BUFFER_COUNT];              // Camera frame buffers (mmap)
    size_t cap_buffer_len_[CAM_BUFFER_COUNT];
    uint8_t* m2m_cap_buffer_[ENCODER_OUTPUT_BUFFERS];    // Encoder output buffers (mmap)
    size_t m2m_cap_buffer_len_[ENCODER_OUTPUT_BUFFERS];

    // Capture task
    TaskHandle_t capture_task_;
    std::atomic<bool> running_;
    FrameCallback frame_callback_;
    std::atomic<bool> force_keyframe_;

    // Performance tracking
    uint64_t start_time_us_;
    uint32_t frame_count_;
    int frames_in_encoder_;  // Pipeline depth tracker

    // Initialization helpers
    bool initCamera();
    bool initEncoder();

    // Capture loop (runs in FreeRTOS task)
    static void captureTaskEntry(void* arg);
    void captureLoop();
};

#endif // ESP32_VIDEO_HPP
