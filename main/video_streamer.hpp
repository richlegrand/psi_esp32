/**
 * VideoStreamer - Complete H.264 video pipeline for WebRTC
 *
 * Consolidates camera capture, H.264 encoding, frame queueing, and RTP transmission
 * into a single cohesive class with front-end frame skipping.
 */

#ifndef VIDEO_STREAMER_HPP
#define VIDEO_STREAMER_HPP

#include "rtc/rtc.hpp"
#include <memory>
#include <atomic>
#include <cstdint>
#include <map>
#include <string>
#include <mutex>

extern "C" {
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
}

class VideoStreamer {
public:
    VideoStreamer(uint32_t width = 1280, uint32_t height = 720, uint32_t fps = 25);
    ~VideoStreamer();

    // Add a track to send video to
    // Automatically starts streaming if this is the first track
    // Returns true on success
    bool addTrack(const std::string& client_id, std::shared_ptr<rtc::Track> track);

    // Remove a track
    // Automatically stops streaming if this was the last track
    void removeTrack(const std::string& client_id);

    // Check if streaming is active
    bool isRunning() const { return running_; }

    uint32_t getWidth() const { return width_; }
    uint32_t getHeight() const { return height_; }
    uint32_t getFPS() const { return fps_; }

private:
    // Configuration
    uint32_t width_;
    uint32_t height_;
    uint32_t fps_;

    // Device file descriptors
    int cap_fd_;       // Camera capture device
    int m2m_fd_;       // H.264 encoder device

    // Camera buffers
    static constexpr int CAM_BUFFER_COUNT = 4;
    uint8_t* cap_buffer_[CAM_BUFFER_COUNT];
    size_t cap_buffer_len_[CAM_BUFFER_COUNT];

    // Encoder output buffers
    static constexpr int ENCODER_OUTPUT_BUFFERS = 3;
    uint8_t* m2m_cap_buffer_[ENCODER_OUTPUT_BUFFERS];
    size_t m2m_cap_buffer_len_[ENCODER_OUTPUT_BUFFERS];

    // Send queue (for async pipelining)
    static constexpr int SEND_QUEUE_DEPTH = 8;  // ~320ms buffering at 25fps
    struct QueuedFrame {
        std::vector<uint8_t> data;
        rtc::FrameInfo info;
    };
    QueueHandle_t send_queue_;

    // Tasks
    TaskHandle_t capture_task_;
    TaskHandle_t send_task_;

    // State
    std::atomic<bool> running_;
    std::atomic<bool> force_keyframe_;

    // Track management (one track per client)
    std::map<std::string, std::shared_ptr<rtc::Track>> tracks_;
    std::mutex tracks_mutex_;

    // Statistics
    uint64_t video_start_pts_;
    uint64_t capture_frame_count_;
    uint32_t frames_in_encoder_;  // Pipeline depth tracker
    uint32_t frames_skipped_;     // Front-end skip counter

    // Initialization
    bool initCamera();
    bool initEncoder();
    void cleanup();

    // Internal start/stop (called by addTrack/removeTrack)
    bool startStreaming();
    void stopStreaming();

    // Capture loop (runs in capture_task_)
    static void captureTaskEntry(void* arg);
    void captureLoop();

    // Send loop (runs in send_task_)
    static void sendTaskEntry(void* arg);
    void sendLoop();

    // Queue depth check for front-end frame skipping
    bool shouldSkipFrame() const;
};

#endif // VIDEO_STREAMER_HPP
