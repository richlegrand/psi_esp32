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
#include "driver/ppa.h"
#include "esp_heap_caps.h"
}

#include "jpeg_decoder.hpp"
#include "jpeg_frame_reader.hpp"
#include "color_teacher.hpp"

enum class VideoSource {
    CAMERA,
    JPEG_FILES
};

// Abstraction for acquired frames (from camera or JPEG decode)
struct AcquiredFrame {
    uint8_t* yuv_data;      // Pointer to YUV420 data
    uint32_t width;         // Frame width
    uint32_t height;        // Frame height
    int buffer_index;       // Camera buffer index (for QBUF on release), -1 for JPEG
    VideoSource source;     // Where this frame came from
};

class VideoStreamer {
public:
    // Constructor
    // output_width/output_height: Desired output resolution
    //   - Camera resolution is auto-detected from sensor (via menuconfig setting)
    //   - PPA scaling automatically enabled if output != camera resolution
    // fps: Frame rate
    // enable_cv: Enable CV pipeline (YUV→RGB→CV→RGB→YUV) vs direct scaling (YUV→YUV)
    VideoStreamer(uint32_t output_width, uint32_t output_height, uint32_t fps = 25, bool enable_cv = false);
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

    // Get output video dimensions
    uint32_t getWidth() const { return output_width_; }
    uint32_t getHeight() const { return output_height_; }
    uint32_t getFPS() const { return fps_; }

    // Freeze mode control - for region selection in browser
    // When frozen, the same frame is sent repeatedly and RGB buffer is preserved
    void requestFreeze();                   // Request freeze on next frame
    bool isFrozen() const { return frozen_; }
    void unfreeze();                        // Exit freeze mode, resume live video

    // Access RGB buffer (only valid when frozen and CV pipeline enabled)
    uint8_t* getRgbBuffer() const { return rgb_buffer_; }
    size_t getRgbBufferSize() const { return rgb_buffer_size_; }
    bool isCvPipelineEnabled() const { return cv_pipeline_enabled_; }

    // Color teaching control
    // Start teaching with ROI (normalized 0-1 coordinates)
    bool startTeaching(float roi_x, float roi_y, float roi_w, float roi_h);
    // Perform one teaching iteration
    bool iterateTeaching();
    // Accept current color model
    void acceptTeaching();
    // Cancel teaching
    void cancelTeaching();
    // Get teaching state
    tracking::TeachState getTeachState() const;
    // Get current color model (only valid after teaching accepted)
    const tracking::ColorModel& getColorModel() const;

private:
    // Configuration
    uint32_t cam_width_;       // Camera resolution (auto-detected from sensor)
    uint32_t cam_height_;
    uint32_t output_width_;    // Desired output resolution
    uint32_t output_height_;
    uint32_t fps_;
    bool use_ppa_;             // True if PPA scaling needed (output != camera)
    bool cv_pipeline_enabled_; // True to use YUV→RGB→CV→YUV pipeline, false for direct YUV→YUV
    VideoSource video_source_; // Detected at runtime (CAMERA or JPEG_FILES)

    // Device file descriptors
    int cap_fd_;       // Camera capture device
    int m2m_fd_;       // H.264 encoder device

    // PPA (Pixel Processing Accelerator) - Dual pipeline for CV
    ppa_client_handle_t ppa_yuv_to_rgb_;   // YUV420 → RGB888 (with optional scaling)
    ppa_client_handle_t ppa_rgb_to_yuv_;   // RGB888 → YUV420 (after CV processing)
    uint8_t* rgb_buffer_;                   // RGB888 buffer for CV processing (in PSRAM)
    size_t rgb_buffer_size_;
    uint8_t* yuv_output_buffer_;            // YUV420 buffer after RGB→YUV conversion
    size_t yuv_output_buffer_size_;

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

    // PPA non-blocking synchronization
    SemaphoreHandle_t ppa_done_sem_;

    // Tasks
    TaskHandle_t capture_task_;
    TaskHandle_t send_task_;

    // State
    std::atomic<bool> running_;
    std::atomic<bool> force_keyframe_;

    // Freeze mode state (for region selection)
    std::atomic<bool> freeze_requested_;    // Set by requestFreeze(), cleared by captureLoop
    std::atomic<bool> frozen_;              // True while video is frozen

    // Color teaching
    tracking::ColorTeacher color_teacher_;
    std::atomic<bool> teach_iterate_requested_;   // Request next iteration
    std::atomic<bool> teach_accept_requested_;    // Request accept model
    std::atomic<bool> visualization_dirty_;       // RGB buffer modified, needs re-encode
    uint8_t* rgb_original_;                       // Copy of original RGB for teaching iterations
    size_t rgb_original_size_;

    // Track management (one track per client)
    std::map<std::string, std::shared_ptr<rtc::Track>> tracks_;
    std::mutex tracks_mutex_;

    // Statistics
    uint64_t video_start_pts_;
    uint64_t capture_frame_count_;
    uint32_t frames_in_encoder_;  // Pipeline depth tracker
    uint32_t frames_skipped_;     // Front-end skip counter

    // JPEG playback mode (initialized only if detected at runtime)
    std::unique_ptr<JpegFrameReader> jpeg_reader_;
    std::unique_ptr<JpegDecoder> jpeg_decoder_;
    std::vector<uint8_t> jpeg_buffer_;      // Read buffer
    uint8_t* decoded_yuv_buffer_;           // JPEG decode output
    size_t decoded_yuv_buffer_size_;

    // Initialization
    VideoSource detectVideoSource();  // Runtime detection of video source
    bool initCamera();
    bool initEncoder();
    bool initPPA();
    bool initJpegMode();
    void cleanup();

    // Internal start/stop (called by addTrack/removeTrack)
    bool startStreaming();
    void stopStreaming();

    // Capture loop (runs in capture_task_)
    static void captureTaskEntry(void* arg);
    void captureLoop();

    // CV processing hook (runs on RGB888 buffer)
    void processCVFrame(uint8_t* rgb_data, uint32_t width, uint32_t height);

    // Test processing (PSRAM bandwidth test)
    uint32_t testProcessing();

    // Send loop (runs in send_task_)
    static void sendTaskEntry(void* arg);
    void sendLoop();

    // Queue depth check for front-end frame skipping
    bool shouldSkipFrame() const;

    // PPA callback for non-blocking mode
    static bool ppaDoneCallback(ppa_client_handle_t ppa_client, ppa_event_data_t* event_data, void* user_data);

    // PPA configuration helpers (reduce code duplication)
    ppa_srm_oper_config_t buildPpaYuvToRgb(uint8_t* src, uint32_t src_w, uint32_t src_h,
                                           uint8_t* dst, size_t dst_size, uint32_t dst_w, uint32_t dst_h,
                                           bool blocking = true);
    ppa_srm_oper_config_t buildPpaRgbToYuv(uint8_t* src, uint32_t src_w, uint32_t src_h,
                                           uint8_t* dst, size_t dst_size,
                                           bool blocking = true);
    ppa_srm_oper_config_t buildPpaYuvScale(uint8_t* src, uint32_t src_w, uint32_t src_h,
                                           uint8_t* dst, size_t dst_size, uint32_t dst_w, uint32_t dst_h,
                                           bool blocking = true);

    // Frame acquisition abstraction (camera or JPEG)
    bool acquireFrame(AcquiredFrame& frame);
    void releaseFrame(AcquiredFrame& frame);

    // Processing paths (output to yuv_output_buffer_)
    bool processFrameCV(const AcquiredFrame& frame);
    bool processFrameDirect(const AcquiredFrame& frame);

    // Encoder helpers
    bool submitToEncoder();
    bool dequeueEncodedFrame(bool blocking, struct v4l2_buffer& enc_output_buf);
    void queueFrameForSending(const struct v4l2_buffer& enc_output_buf);
};

#endif // VIDEO_STREAMER_HPP
